#include "SegmentedDownloader.h"
#include "PathUtils.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrlQuery>
#include <QDateTime>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

bool SegmentedDownloader::writeFully(int fd, const char *data, qint64 size, off64_t offset, int *errOut) {
    while (size > 0) {
        const ssize_t n = ::pwrite64(fd, data, static_cast<size_t>(size), offset);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errOut) *errOut = errno;
            return false;
        }
        if (n == 0) {
            if (errOut) *errOut = EIO;
            return false;
        }
        data += n;
        size -= n;
        offset += n;
    }
    return true;
}

bool SegmentedDownloader::validateContentRangeHeader(const QString &header,
                                                     qint64 expectedStart,
                                                     qint64 segEnd,
                                                     qint64 totalBytes,
                                                     qint64 *rangeFirstOut,
                                                     qint64 *rangeLastOut) {
    static const QRegularExpression re(QStringLiteral(R"(^bytes\s+(\d+)-(\d+)/(\d+|\*)$)"), QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = re.match(header.trimmed());
    if (!match.hasMatch()) {
        return false;
    }

    qint64 first = match.captured(1).toLongLong();
    qint64 last = match.captured(2).toLongLong();

    if (rangeFirstOut) *rangeFirstOut = first;
    if (rangeLastOut) *rangeLastOut = last;

    if (expectedStart >= 0 && first != expectedStart) {
        return false;
    }

    if (segEnd >= 0 && last > segEnd) {
        return false;
    }

    if (first > last) {
        return false;
    }

    QString totalStr = match.captured(3);
    if (totalStr != QLatin1String("*") && totalBytes > 0) {
        if (totalStr.toLongLong() != totalBytes) {
            return false;
        }
    }

    return true;
}

SegmentedDownloader::SegmentedDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent),
      m_item(item),
      m_nam(new QNetworkAccessManager(this)),
      m_probeReply(NULL),
      m_singleReply(NULL),
      m_singleValidated(false),
      m_singleLastActivity(0),
      m_singleRetries(0),
      m_fileFd(-1),
      m_stopped(false),
      m_finished(false),
      m_speedTimer(new QTimer(this)),
      m_bytesInInterval(0),
      m_lastReportTime(0) {
    connect(m_speedTimer, SIGNAL(timeout()), this, SLOT(onSpeedTimerTimeout()));
}

SegmentedDownloader::~SegmentedDownloader() {
    cancel();
}

void SegmentedDownloader::start() {
    m_stopped = false;
    m_finished = false;
    m_elapsedTimer.start();
    m_speedTimer->start(500);

    bool canResume = (m_item.m_totalBytes > 0)
                     && (m_item.m_supportsRanges)
                     && (!m_item.m_segments.isEmpty())
                     && QFile::exists(m_item.partFilePath());

    if (canResume) {
        if (!openPartFile(false)) {
            if (!m_finished) {
                failDownload(tr("Parça dosyası açılamadı"));
            }
            return;
        }
        m_item.m_state = DownloadStateDownloading;
        startMultiSegment();
    } else {
        m_item.m_state = DownloadStateConnecting;
        probe();
    }
}

void SegmentedDownloader::pause() {
    if (m_stopped || m_finished) return;
    m_stopped = true;
    m_speedTimer->stop();

    if (m_probeReply != NULL) {
        m_probeReply->disconnect(this);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply = NULL;
    }

    if (m_singleReply != NULL) {
        m_singleReply->disconnect(this);
        m_singleReply->abort();
        m_singleReply->deleteLater();
        m_singleReply = NULL;
    }

    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    m_segmentReplies.clear();
    m_replyExpectedStart.clear();
    m_validatedReplies.clear();
    m_replyLastActivity.clear();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply != NULL) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    closePartFile();
    m_item.m_state = DownloadStatePaused;
    m_item.m_speed = 0.0;
    emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, 0.0, m_item.m_segments);
}

void SegmentedDownloader::cancel() {
    pause();
    m_item.m_state = DownloadStateCancelled;
}

QNetworkRequest SegmentedDownloader::createRequest(const QUrl &url, const QString &rangeHeader) const {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setRawHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 HBDownloadManager/1.3");
    req.setRawHeader("Accept-Encoding", "identity");

    if (!m_item.m_cookieHeader.isEmpty()) {
        req.setRawHeader("Cookie", m_item.m_cookieHeader.toUtf8());
    }

    for (QMap<QString, QString>::const_iterator it = m_item.m_headers.constBegin(); it != m_item.m_headers.constEnd(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    if (!rangeHeader.isEmpty()) {
        req.setRawHeader("Range", rangeHeader.toUtf8());
    }

    return req;
}

void SegmentedDownloader::probe() {
    QNetworkRequest req = createRequest(m_item.m_url, "bytes=0-0");
    m_probeReply = m_nam->get(req);
    connect(m_probeReply, SIGNAL(metaDataChanged()), this, SLOT(onProbeMetaDataChanged()));
    connect(m_probeReply, SIGNAL(finished()), this, SLOT(onProbeFinished()));
}

void SegmentedDownloader::onProbeMetaDataChanged() {
    if (m_probeReply == NULL) return;
    QNetworkReply *reply = m_probeReply;

    if (m_stopped || m_finished) return;

    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 0) return;

    // Disconnect so finished() doesn't fire when we abort()
    reply->disconnect(this);
    m_probeReply = NULL;

    QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid()) {
        QUrl nextUrl = m_item.m_url.resolved(redirect.toUrl());
        m_item.m_url = nextUrl;
        reply->abort();
        reply->deleteLater();
        probe();
        return;
    }

    if (statusCode == 401 || statusCode == 403) {
        reply->abort();
        reply->deleteLater();
        failDownload(tr("Erişim engellendi veya yetkilendirme gerekli (HTTP %1)").arg(statusCode));
        return;
    }

    if (statusCode == 404 || statusCode == 410) {
        reply->abort();
        reply->deleteLater();
        failDownload(tr("Dosya bulunamadı (HTTP %1)").arg(statusCode));
        return;
    }

    if (statusCode == 416 || statusCode == 501) {
        reply->abort();
        reply->deleteLater();
        m_item.m_supportsRanges = false;
        m_item.m_totalBytes = -1;
        if (openPartFile(false)) {
            m_item.m_state = DownloadStateDownloading;
            startSingleStream();
            return;
        } else {
            if (!m_finished) {
                failDownload(tr("Part dosyası oluşturulamadı"));
            }
            return;
        }
    }

    handleProbeReply(reply);
    reply->abort();
    reply->deleteLater();
}

void SegmentedDownloader::onProbeFinished() {
    if (m_probeReply == NULL) return;
    QNetworkReply *reply = m_probeReply;
    m_probeReply = NULL;
    reply->disconnect(this);
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    if (reply->error() != QNetworkReply::NoError) {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 416 || status == 501) {
            m_item.m_supportsRanges = false;
            m_item.m_totalBytes = -1;
            if (openPartFile(false)) {
                m_item.m_state = DownloadStateDownloading;
                startSingleStream();
                return;
            }
        }
        if (status == 401 || status == 403) {
            failDownload(tr("Erişim engellendi veya yetkilendirme gerekli (HTTP %1)").arg(status));
            return;
        }
        if (status == 404 || status == 410) {
            failDownload(tr("Dosya bulunamadı (HTTP %1)").arg(status));
            return;
        }
        failDownload(reply->errorString());
        return;
    }

    // In case metaDataChanged didn't fire, evaluate here
    handleProbeReply(reply);
}

void SegmentedDownloader::handleProbeReply(QNetworkReply *reply) {
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode < 200 || statusCode >= 300) {
        failDownload(tr("Sunucu hatası (HTTP %1)").arg(statusCode));
        return;
    }

    QVariant redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute);
    if (redirect.isValid()) {
        QUrl nextUrl = m_item.m_url.resolved(redirect.toUrl());
        m_item.m_url = nextUrl;
        probe();
        return;
    }

    qint64 total = -1;
    bool ranges = false;

    if (statusCode == 206) {
        static const QRegularExpression crRegex(QStringLiteral(R"(bytes\s+\d+-\d+/(\d+|\*))"), QRegularExpression::CaseInsensitiveOption);
        QString contentRange = QString::fromUtf8(reply->rawHeader("Content-Range"));
        QRegularExpressionMatch match = crRegex.match(contentRange);
        if (match.hasMatch()) {
            QString lenStr = match.captured(1);
            if (lenStr != QLatin1String("*")) {
                total = lenStr.toLongLong();
                ranges = true;
            }
        }
    } else if (statusCode == 200) {
        // Server responded 200 to Range request -> ignored Range!
        if (reply->hasRawHeader("Content-Length")) {
            total = QString::fromUtf8(reply->rawHeader("Content-Length")).toLongLong();
        }
        ranges = false;
    }

    QString contentDisp = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    QString suggestedName = parseContentDisposition(contentDisp);
    if (suggestedName.isEmpty()) {
        suggestedName = QFileInfo(m_item.m_url.path()).fileName();
    }
    suggestedName = PathUtils::sanitizeFileName(suggestedName);

    if (reply->url().isValid()) {
        m_item.m_url = reply->url();
    }

    if (m_item.m_fileName.isEmpty() || m_item.m_fileName == "download") {
        m_item.m_fileName = suggestedName;
    }
    m_item.m_totalBytes = total;
    m_item.m_supportsRanges = ranges;

    // Check for HTML response when expecting a non-HTML file
    QString contentType = QString::fromUtf8(reply->rawHeader("Content-Type")).toLower();
    QString lowerName = m_item.m_fileName.toLower();
    if (contentType.startsWith("text/html") && !lowerName.endsWith(".html") && !lowerName.endsWith(".htm")) {
        failDownload(tr("Bağlantı süresi dolmuş veya oturum açma sayfası döndü (Content-Type: text/html)"));
        return;
    }

    ProbeResult res;
    res.m_totalBytes = total;
    res.m_supportsRanges = ranges;
    res.m_suggestedFileName = m_item.m_fileName;
    res.m_contentType = QString::fromUtf8(reply->rawHeader("Content-Type"));
    res.m_finalUrl = reply->url();
    emit atProbed(res);

    qint64 minSegmentSize = 512 * 1024;
    int targetSegments = m_item.m_requestedSegments > 0 ? m_item.m_requestedSegments : 8;
    int count = ranges ? qMax(1, qMin(targetSegments, static_cast<int>(total / minSegmentSize))) : 1;

    if (ranges && count >= 1 && total > 0) {
        m_item.m_segments.clear();
        qint64 segSize = total / count;
        for (int i = 0; i < count; ++i) {
            SegmentInfo s;
            s.m_index = i;
            s.m_start = i * segSize;
            s.m_end = (i == count - 1) ? (total - 1) : (s.m_start + segSize - 1);
            s.m_received = 0;
            m_item.m_segments.append(s);
        }

        if (!openPartFile(true)) {
            if (!m_finished) {
                failDownload(tr("Part dosyası oluşturulamadı"));
            }
            return;
        }
        m_item.m_state = DownloadStateDownloading;
        startMultiSegment();
    } else {
        if (!openPartFile(false)) {
            if (!m_finished) {
                failDownload(tr("Part dosyası oluşturulamadı"));
            }
            return;
        }
        m_item.m_state = DownloadStateDownloading;
        startSingleStream();
    }
}

bool SegmentedDownloader::openPartFile(bool preallocate) {
    QDir dir(m_item.m_directory);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString partPath = m_item.partFilePath();
    m_fileFd = ::open(partPath.toUtf8().constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (m_fileFd < 0) {
        return false;
    }

    if (preallocate && m_item.m_totalBytes > 0) {
        int ret = ::posix_fallocate(m_fileFd, 0, m_item.m_totalBytes);
        if (ret != 0) {
            if (ret == ENOSPC) {
                failDownload(tr("Yetersiz disk alanı"));
                return false;
            } else if (ret == EFBIG) {
                failDownload(tr("Dosya çok büyük (dosya sistemi sınırı)"));
                return false;
            } else {
                qWarning() << "posix_fallocate returned error:" << ret;
            }
        }
    }

    return true;
}

void SegmentedDownloader::closePartFile() {
    if (m_fileFd >= 0) {
        ::close(m_fileFd);
        m_fileFd = -1;
    }
}

void SegmentedDownloader::startMultiSegment() {
    for (int i = 0; i < m_item.m_segments.size(); ++i) {
        if (!m_item.m_segments.at(i).isDone()) {
            launchSegment(i);
        }
    }
}

void SegmentedDownloader::launchSegment(int index) {
    if (m_stopped || m_finished) return;
    const SegmentInfo &seg = m_item.m_segments.at(index);
    qint64 currentOffset = seg.m_start + seg.m_received;
    if (currentOffset > seg.m_end) return;

    QString rangeStr = QString("bytes=%1-%2").arg(currentOffset).arg(seg.m_end);
    QNetworkRequest req = createRequest(m_item.m_url, rangeStr);

    QNetworkReply *reply = m_nam->get(req);
    m_segmentReplies[reply] = index;
    m_replyExpectedStart[reply] = currentOffset;
    m_replyLastActivity[reply] = QDateTime::currentMSecsSinceEpoch();

    connect(reply, SIGNAL(readyRead()), this, SLOT(onSegmentReadyRead()));
    connect(reply, SIGNAL(finished()), this, SLOT(onSegmentFinished()));
}

bool SegmentedDownloader::validateSegmentReply(QNetworkReply *reply, const SegmentInfo &seg) {
    if (reply == NULL) return false;

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // If status is 200 on Range request: fallback to single stream from byte 0
    if (status == 200) {
        qWarning() << "Server returned HTTP 200 on Range request. Falling back to single stream.";
        m_item.m_supportsRanges = false;

        QList<QNetworkReply*> replies = m_segmentReplies.keys();
        m_segmentReplies.clear();
        m_replyExpectedStart.clear();
        m_validatedReplies.clear();
        m_replyLastActivity.clear();
        for (int i = 0; i < replies.size(); ++i) {
            QNetworkReply *r = replies.at(i);
            if (r != NULL) {
                r->disconnect(this);
                r->abort();
                r->deleteLater();
            }
        }

        m_item.m_segments.clear();
        m_item.m_receivedBytes = 0;
        if (m_fileFd >= 0) {
            if (::ftruncate(m_fileFd, 0) != 0) {
                qWarning() << "ftruncate failed:" << errno;
            }
            ::lseek(m_fileFd, 0, SEEK_SET);
        }

        startSingleStream();
        return false;
    }

    if (status == 401 || status == 403 || status == 404 || status == 410) {
        failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
        return false;
    }

    if (status != 206) {
        int segIndex = m_segmentReplies.value(reply, -1);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        m_segmentReplies.remove(reply);
        m_replyExpectedStart.remove(reply);
        m_validatedReplies.remove(reply);
        m_replyLastActivity.remove(reply);

        if (segIndex >= 0) {
            scheduleRetry(segIndex, tr("Geçersiz HTTP yanıtı: %1").arg(status));
        } else {
            failDownload(tr("Geçersiz HTTP yanıtı: %1").arg(status));
        }
        return false;
    }

    // Content-Range: bytes <first>-<last>/<total|*>
    QString crHeader = QString::fromLatin1(reply->rawHeader("Content-Range")).trimmed();
    qint64 expectedStart = m_replyExpectedStart.value(reply, -1);
    qint64 first = -1;
    qint64 last = -1;

    if (!validateContentRangeHeader(crHeader, expectedStart, seg.m_end, m_item.m_totalBytes, &first, &last)) {
        qWarning() << "Invalid Content-Range header or mismatch:" << crHeader;
        int segIndex = m_segmentReplies.value(reply, -1);
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        m_segmentReplies.remove(reply);
        m_replyExpectedStart.remove(reply);
        m_validatedReplies.remove(reply);
        m_replyLastActivity.remove(reply);

        if (segIndex >= 0) {
            scheduleRetry(segIndex, tr("Geçersiz Content-Range başlığı veya aralık uyuşmazlığı"));
        }
        return false;
    }

    return true;
}

void SegmentedDownloader::onSegmentReadyRead() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL || !m_segmentReplies.contains(reply)) return;
    if (m_fileFd < 0) return;

    int segIndex = m_segmentReplies.value(reply);
    SegmentInfo &seg = m_item.m_segments[segIndex];

    if (!m_validatedReplies.contains(reply)) {
        if (!validateSegmentReply(reply, seg)) {
            return;
        }
        m_validatedReplies.insert(reply);
    }

    QByteArray data = reply->readAll();
    if (data.isEmpty()) return;

    qint64 remaining = seg.length() - seg.m_received;
    if (remaining <= 0) return;

    qint64 toWrite = qMin(static_cast<qint64>(data.size()), remaining);
    off64_t fileOffset = seg.m_start + seg.m_received;

    int err = 0;
    if (!writeFully(m_fileFd, data.constData(), toWrite, fileOffset, &err)) {
        failDownload(tr("Dosyaya yazılamadı: %1").arg(QString::fromLocal8Bit(strerror(err))));
        return;
    }

    seg.m_received += toWrite;
    m_item.m_receivedBytes += toWrite;
    m_bytesInInterval += toWrite;

    // Segment made progress: reset retry counter & update activity timestamp
    m_segmentRetries[segIndex] = 0;
    m_replyLastActivity[reply] = QDateTime::currentMSecsSinceEpoch();
}

void SegmentedDownloader::onSegmentFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL) return;

    if (!m_segmentReplies.contains(reply)) {
        reply->deleteLater();
        return;
    }

    int segIndex = m_segmentReplies.take(reply);
    m_replyExpectedStart.remove(reply);
    m_validatedReplies.remove(reply);
    m_replyLastActivity.remove(reply);
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    const SegmentInfo &seg = m_item.m_segments.at(segIndex);
    if (reply->error() != QNetworkReply::NoError || !seg.isDone()) {
        scheduleRetry(segIndex, reply->error() != QNetworkReply::NoError ? reply->errorString() : tr("Segment eksik tamamlandı"));
        return;
    }

    checkAllSegmentsCompleted();
}

void SegmentedDownloader::scheduleRetry(int segIndex, const QString &reason) {
    if (m_stopped || m_finished) return;

    int retries = m_segmentRetries.value(segIndex, 0) + 1;
    m_segmentRetries[segIndex] = retries;

    if (retries <= s_maxRetries) {
        int delayMs = 1000 * retries;
        QTimer::singleShot(delayMs, this, [this, segIndex]() {
            if (!m_stopped && !m_finished) {
                launchSegment(segIndex);
            }
        });
    } else {
        failDownload(tr("Segment %1 indirilemedi (%2 deneme): %3")
            .arg(segIndex + 1).arg(s_maxRetries).arg(reason));
    }
}

void SegmentedDownloader::startSingleStream() {
    QString rangeHeader;
    if (m_item.m_supportsRanges && m_item.m_receivedBytes > 0) {
        rangeHeader = QString("bytes=%1-").arg(m_item.m_receivedBytes);
    } else {
        m_item.m_receivedBytes = 0;
        if (m_fileFd >= 0) {
            if (::ftruncate(m_fileFd, 0) != 0) {
                qWarning() << "ftruncate failed:" << errno;
            }
            ::lseek(m_fileFd, 0, SEEK_SET);
        }
    }

    m_singleValidated = false;
    QNetworkRequest req = createRequest(m_item.m_url, rangeHeader);
    m_singleReply = m_nam->get(req);
    m_singleLastActivity = QDateTime::currentMSecsSinceEpoch();

    connect(m_singleReply, SIGNAL(readyRead()), this, SLOT(onSingleReadyRead()));
    connect(m_singleReply, SIGNAL(finished()), this, SLOT(onSingleFinished()));
}

void SegmentedDownloader::onSingleReadyRead() {
    if (m_singleReply == NULL || m_fileFd < 0) return;

    if (!m_singleValidated) {
        m_singleValidated = true;
        int status = m_singleReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 401 || status == 403 || status == 404 || status == 410) {
            failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
            return;
        }

        if (status == 200 && m_item.m_receivedBytes > 0) {
            // Server ignored Range request on resume: reset and restart from 0
            m_item.m_supportsRanges = false;
            m_item.m_receivedBytes = 0;
            if (m_fileFd >= 0) {
                if (::ftruncate(m_fileFd, 0) != 0) {
                qWarning() << "ftruncate failed:" << errno;
            }
                ::lseek(m_fileFd, 0, SEEK_SET);
            }
        } else if (status != 200 && status != 206) {
            failDownload(tr("Sunucu yanıtı geçersiz (HTTP %1)").arg(status));
            return;
        }

        QString contentType = QString::fromUtf8(m_singleReply->rawHeader("Content-Type")).toLower();
        QString lowerName = m_item.m_fileName.toLower();
        if (contentType.startsWith("text/html") && !lowerName.endsWith(".html") && !lowerName.endsWith(".htm")) {
            failDownload(tr("Bağlantı süresi dolmuş veya oturum açma sayfası döndü (Content-Type: text/html)"));
            return;
        }
    }

    QByteArray data = m_singleReply->readAll();
    if (data.isEmpty()) return;

    int err = 0;
    if (!writeFully(m_fileFd, data.constData(), data.size(), m_item.m_receivedBytes, &err)) {
        failDownload(tr("Dosyaya yazılamadı: %1").arg(QString::fromLocal8Bit(strerror(err))));
        return;
    }

    m_item.m_receivedBytes += data.size();
    m_bytesInInterval += data.size();
    m_singleRetries = 0;
    m_singleLastActivity = QDateTime::currentMSecsSinceEpoch();
}

void SegmentedDownloader::onSingleFinished() {
    if (m_singleReply == NULL) return;
    QNetworkReply *reply = m_singleReply;
    m_singleReply = NULL;
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    if (reply->error() != QNetworkReply::NoError) {
        if (reply->error() == QNetworkReply::OperationCanceledError && (m_stopped || m_finished)) {
            return;
        }
        m_singleRetries++;
        if (m_singleRetries <= s_maxRetries) {
            int delayMs = 1000 * m_singleRetries;
            QTimer::singleShot(delayMs, this, [this]() {
                if (!m_stopped && !m_finished) {
                    startSingleStream();
                }
            });
            return;
        } else {
            failDownload(reply->errorString());
            return;
        }
    }

    if (m_item.m_totalBytes > 0 && m_item.m_receivedBytes < m_item.m_totalBytes) {
        m_singleRetries++;
        if (m_singleRetries <= s_maxRetries) {
            int delayMs = 1000 * m_singleRetries;
            QTimer::singleShot(delayMs, this, [this]() {
                if (!m_stopped && !m_finished) {
                    startSingleStream();
                }
            });
            return;
        } else {
            failDownload(tr("İndirme eksik tamamlandı"));
            return;
        }
    }

    if (m_item.m_totalBytes <= 0) {
        m_item.m_totalBytes = m_item.m_receivedBytes;
    }

    finishDownload();
}

void SegmentedDownloader::checkAllSegmentsCompleted() {
    if (m_item.m_segments.isEmpty()) return;

    bool allDone = true;
    for (int i = 0; i < m_item.m_segments.size(); ++i) {
        if (!m_item.m_segments.at(i).isDone()) {
            allDone = false;
            break;
        }
    }

    if (allDone) {
        finishDownload();
    }
}

void SegmentedDownloader::applyUniqueFilename() {
    QString target = PathUtils::safeJoin(m_item.m_directory, m_item.m_fileName);
    if (target.isEmpty()) {
        target = QDir(m_item.m_directory).filePath(m_item.m_fileName);
    }
    if (!QFile::exists(target)) {
        m_item.m_finalPath = target;
        return;
    }

    QFileInfo info(target);
    QString baseName = info.completeBaseName();
    QRegularExpression countRegex(R"(\s\(\d+\)$)");
    baseName.remove(countRegex);
    QString suffix = info.suffix();
    if (!suffix.isEmpty()) suffix = "." + suffix;

    int counter = 1;
    while (true) {
        QString candidate = PathUtils::safeJoin(m_item.m_directory, QString("%1 (%2)%3").arg(baseName).arg(counter).arg(suffix));
        if (candidate.isEmpty()) {
            candidate = QDir(m_item.m_directory).filePath(QString("%1 (%2)%3").arg(baseName).arg(counter).arg(suffix));
        }
        if (!QFile::exists(candidate)) {
            m_item.m_finalPath = candidate;
            m_item.m_fileName = QFileInfo(candidate).fileName();
            return;
        }
        counter++;
    }
}

void SegmentedDownloader::finishDownload() {
    if (m_finished) return;
    m_finished = true;
    m_speedTimer->stop();
    closePartFile();

    applyUniqueFilename();

    QString partPath = m_item.partFilePath();
    bool renameOk = false;

    if (QFile::exists(partPath)) {
        if (QFile::rename(partPath, m_item.m_finalPath)) {
            renameOk = true;
        } else {
            // Target exists or is locked. Pick next 'name (N).ext' and retry.
            QFileInfo info(m_item.m_finalPath);
            QString baseName = info.completeBaseName();
            QRegularExpression countRegex(R"(\s\(\d+\)$)");
            baseName.remove(countRegex);
            QString suffix = info.suffix();
            if (!suffix.isEmpty()) suffix = "." + suffix;

            for (int counter = 1; counter <= 1000; ++counter) {
                QString candidate = PathUtils::safeJoin(m_item.m_directory, QString("%1 (%2)%3").arg(baseName).arg(counter).arg(suffix));
                if (candidate.isEmpty()) {
                    candidate = QDir(m_item.m_directory).filePath(QString("%1 (%2)%3").arg(baseName).arg(counter).arg(suffix));
                }
                if (!QFile::exists(candidate) && QFile::rename(partPath, candidate)) {
                    m_item.m_finalPath = candidate;
                    m_item.m_fileName = QFileInfo(candidate).fileName();
                    renameOk = true;
                    break;
                }
            }
        }
    } else {
        if (m_item.m_totalBytes == 0) {
            if (QFile::exists(m_item.m_finalPath)) {
                renameOk = true;
            } else {
                QFile emptyFile(m_item.m_finalPath);
                if (emptyFile.open(QIODevice::WriteOnly)) {
                    emptyFile.close();
                    renameOk = true;
                }
            }
        }
    }

    if (!renameOk) {
        m_item.m_state = DownloadStateError;
        m_item.m_errorMessage = tr("Parça dosyası hedef dosyaya taşınamadı: %1").arg(m_item.m_finalPath);
        m_item.m_speed = 0.0;
        emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, 0.0, m_item.m_segments);
        emit atFinished(false, m_item.m_errorMessage);
        return;
    }

    m_item.m_state = DownloadStateCompleted;
    m_item.m_completedAt = QDateTime::currentDateTime();
    m_item.m_speed = 0.0;
    emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, 0.0, m_item.m_segments);
    emit atFinished(true, QString());
}

void SegmentedDownloader::failDownload(const QString &error) {
    if (m_finished) return;
    m_finished = true;
    m_speedTimer->stop();

    if (m_probeReply != NULL) {
        m_probeReply->disconnect(this);
        m_probeReply->abort();
        m_probeReply->deleteLater();
        m_probeReply = NULL;
    }

    if (m_singleReply != NULL) {
        m_singleReply->disconnect(this);
        m_singleReply->abort();
        m_singleReply->deleteLater();
        m_singleReply = NULL;
    }

    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    m_segmentReplies.clear();
    m_replyExpectedStart.clear();
    m_validatedReplies.clear();
    m_replyLastActivity.clear();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply != NULL) {
            reply->disconnect(this);
            reply->abort();
            reply->deleteLater();
        }
    }

    closePartFile();

    m_item.m_state = DownloadStateError;
    m_item.m_errorMessage = error;
    m_item.m_speed = 0.0;
    emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, 0.0, m_item.m_segments);
    emit atFinished(false, error);
}

void SegmentedDownloader::onSpeedTimerTimeout() {
    qint64 now = m_elapsedTimer.elapsed();
    qint64 elapsedMs = now - m_lastReportTime;
    if (elapsedMs > 0) {
        double bytesPerSec = (static_cast<double>(m_bytesInInterval) * 1000.0) / static_cast<double>(elapsedMs);
        if (m_item.m_speed <= 0.0) {
            m_item.m_speed = bytesPerSec;
        } else {
            m_item.m_speed = 0.35 * bytesPerSec + 0.65 * m_item.m_speed;
        }
        m_bytesInInterval = 0;
        m_lastReportTime = now;

        emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, m_item.m_speed, m_item.m_segments);
    }

    // Stall detection: check if any reply has had no data for > 30 seconds
    qint64 currentEpochMs = QDateTime::currentMSecsSinceEpoch();
    const qint64 stallThresholdMs = 30000;

    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply != NULL && m_replyLastActivity.contains(reply)) {
            if (currentEpochMs - m_replyLastActivity.value(reply) > stallThresholdMs) {
                qWarning() << "Segment reply stalled for >30s, aborting to trigger retry";
                reply->abort();
            }
        }
    }

    if (m_singleReply != NULL && m_singleLastActivity > 0) {
        if (currentEpochMs - m_singleLastActivity > stallThresholdMs) {
            qWarning() << "Single stream reply stalled for >30s, aborting to trigger retry";
            m_singleReply->abort();
        }
    }
}

QString SegmentedDownloader::parseContentDisposition(const QString &header) {
    if (header.isEmpty()) return QString();

    static QRegularExpression rfc5987Regex(R"(filename\*\s*=\s*UTF-8''([^;]+))", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch match = rfc5987Regex.match(header);
    if (match.hasMatch()) {
        QString encoded = match.captured(1).trimmed();
        return QUrl::fromPercentEncoding(encoded.toUtf8());
    }

    static QRegularExpression quotedRegex("filename\\s*=\\s*\"([^\"]+)\"", QRegularExpression::CaseInsensitiveOption);
    match = quotedRegex.match(header);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    static QRegularExpression bareRegex(R"(filename\s*=\s*([^;]+))", QRegularExpression::CaseInsensitiveOption);
    match = bareRegex.match(header);
    if (match.hasMatch()) {
        return match.captured(1).trimmed();
    }

    return QString();
}
