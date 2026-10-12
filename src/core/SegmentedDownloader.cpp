#include "SegmentedDownloader.h"
#include "PathUtils.h"
#include "RangePlanner.h"
#include <QNetworkCookieJar>
#include <QNetworkCookie>
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
    return RangePlanner::validateContentRangeHeader(header, expectedStart, segEnd, totalBytes, rangeFirstOut, rangeLastOut);
}

QString SegmentedDownloader::uniqueTargetPath(const QString &baseDir, const QString &fileName, const QString &id) {
    return PathUtils::uniqueTargetPath(baseDir, fileName, id);
}

void SegmentedDownloader::syncToDisk() {
    if (m_fileFd >= 0) {
        ::fdatasync(m_fileFd);
    }
}

namespace {
class BridgeCookieJar : public QNetworkCookieJar {
public:
    explicit BridgeCookieJar(QObject *parent = nullptr) : QNetworkCookieJar(parent) {}
    void populate(const QList<QNetworkCookie> &cookies) {
        setAllCookies(cookies);
    }
};
}

SegmentedDownloader::SegmentedDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent),
      m_item(item),
      m_nam(new QNetworkAccessManager(this)),
      m_probeReply(NULL),
      m_singleReply(NULL),
      m_singleValidated(false),
      m_singleLaunchTime(0),
      m_singleLastActivity(0),
      m_singleWithHeaders(false),
      m_singleRetries(0),
      m_singleConnectFailures(0),
      m_fullRestarts(0),
      m_probeRetries(0),
      m_probeLaunchTime(0),
      m_fileFd(-1),
      m_stopped(false),
      m_finished(false),
      m_speedTimer(new QTimer(this)),
      m_bytesInInterval(0),
      m_lastReportTime(0) {
    m_elapsedTimer.start();
    connect(m_speedTimer, SIGNAL(timeout()), this, SLOT(onSpeedTimerTimeout()));

    if (!m_item.m_cookies.isEmpty()) {
        BridgeCookieJar *jar = new BridgeCookieJar(m_nam);
        QList<QNetworkCookie> cookieList;
        for (int i = 0; i < m_item.m_cookies.size(); ++i) {
            QJsonObject cObj = m_item.m_cookies.at(i).toObject();
            QNetworkCookie cookie(cObj["name"].toString().toUtf8(), cObj["value"].toString().toUtf8());
            if (cObj.contains("domain")) cookie.setDomain(cObj["domain"].toString());
            if (cObj.contains("path")) cookie.setPath(cObj["path"].toString());
            if (cObj.contains("secure")) cookie.setSecure(cObj["secure"].toBool());
            if (cObj.contains("httpOnly")) cookie.setHttpOnly(cObj["httpOnly"].toBool());
            cookieList.append(cookie);
        }
        jar->populate(cookieList);
        m_nam->setCookieJar(jar);
    }
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
                     && QFile::exists(m_item.partFilePath())
                     && (QFileInfo(m_item.partFilePath()).size() == m_item.m_totalBytes);

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
    req.setMaximumRedirectsAllowed(10);
    req.setRawHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 HBDownloadManager/1.3");
    req.setRawHeader("Accept-Encoding", "identity");

    if (m_item.m_cookies.isEmpty() && !m_item.m_cookieHeader.isEmpty()) {
        req.setRawHeader("Cookie", m_item.m_cookieHeader.toUtf8());
    }

    for (QMap<QString, QString>::const_iterator it = m_item.m_headers.constBegin(); it != m_item.m_headers.constEnd(); ++it) {
        req.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }

    if (!rangeHeader.isEmpty()) {
        req.setRawHeader("Range", rangeHeader.toUtf8());
        if (!m_item.m_etag.isEmpty()) {
            req.setRawHeader("If-Range", m_item.m_etag.toUtf8());
        }
    }

    return req;
}

void SegmentedDownloader::probe() {
    m_probeLaunchTime = m_elapsedTimer.elapsed();
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
    if (statusCode == 0 || (statusCode >= 300 && statusCode < 400)) return;

    // Disconnect so finished() doesn't fire when we abort()
    reply->disconnect(this);
    m_probeReply = NULL;

    if (reply->url().isValid() && reply->url() != m_item.m_url) {
        m_item.m_url = reply->url();
    }

    if (statusCode >= 500 && statusCode < 600) {
        reply->abort();
        reply->deleteLater();
        if (m_probeRetries < 3) {
            m_probeRetries++;
            if (m_item.m_originalUrl.isValid() && m_item.m_url != m_item.m_originalUrl) {
                m_item.m_url = m_item.m_originalUrl;
            }
            int delayMs = 1000 * m_probeRetries;
            QTimer::singleShot(delayMs, this, [this]() {
                if (!m_stopped && !m_finished) {
                    probe();
                }
            });
            return;
        }
        failDownload(tr("Sunucu hatası (HTTP %1)").arg(statusCode));
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

        // Retry transient network errors or 5xx up to 3 times
        if (m_probeRetries < 3) {
            m_probeRetries++;
            if (m_item.m_originalUrl.isValid() && m_item.m_url != m_item.m_originalUrl) {
                m_item.m_url = m_item.m_originalUrl;
            }
            int delayMs = 1000 * m_probeRetries;
            QTimer::singleShot(delayMs, this, [this]() {
                if (!m_stopped && !m_finished) {
                    probe();
                }
            });
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

    if (reply->hasRawHeader("ETag")) {
        m_item.m_etag = QString::fromUtf8(reply->rawHeader("ETag")).trimmed();
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
    targetSegments = qBound(1, targetSegments, 16);

    if (ranges && total > 0) {
        m_item.m_receivedBytes = 0; // D4: reset before planning!
        m_item.m_segments = RangePlanner::planAdaptive(total, targetSegments, minSegmentSize, ranges);

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
        if (::ftruncate(m_fileFd, m_item.m_totalBytes) != 0) {
            qWarning() << "ftruncate before fallocate failed:" << errno;
        }
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
    m_pendingSegments.clear();
    for (int i = 0; i < m_item.m_segments.size(); ++i) {
        if (!m_item.m_segments.at(i).isDone()) {
            m_pendingSegments.append(i);
        }
    }
    launchPending();
}

void SegmentedDownloader::launchPending() {
    while (!m_stopped && !m_finished
           && m_segmentReplies.size() < s_maxParallelReplies
           && !m_pendingSegments.isEmpty()) {
        launchSegment(m_pendingSegments.takeFirst());
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
    qint64 now = m_elapsedTimer.elapsed();
    m_replyLaunchTime[reply] = now;
    m_replyLastActivity[reply] = now;

    connect(reply, SIGNAL(metaDataChanged()), this, SLOT(onSegmentMetaDataChanged()));
    connect(reply, SIGNAL(readyRead()), this, SLOT(onSegmentReadyRead()));
    connect(reply, SIGNAL(finished()), this, SLOT(onSegmentFinished()));
}

void SegmentedDownloader::onSegmentMetaDataChanged() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL || !m_segmentReplies.contains(reply)) return;
    m_repliesWithHeaders.insert(reply);
    m_replyLastActivity[reply] = m_elapsedTimer.elapsed();
}

bool SegmentedDownloader::validateSegmentReply(QNetworkReply *reply, const SegmentInfo &seg) {
    if (reply == NULL) return false;

    int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // If status is 200 on Range request: fallback to single stream from byte 0
    if (status == 200) {
        qWarning() << "Server returned HTTP 200 on Range request. Falling back to single stream.";
        m_item.m_supportsRanges = false;
        m_pendingSegments.clear();

        QList<QNetworkReply*> replies = m_segmentReplies.keys();
        m_segmentReplies.clear();
        m_replyExpectedStart.clear();
        m_validatedReplies.clear();
        m_replyLaunchTime.clear();
        m_replyLastActivity.clear();
        m_repliesWithHeaders.clear();
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
        m_replyLaunchTime.remove(reply);
        m_replyLastActivity.remove(reply);
        m_repliesWithHeaders.remove(reply);

        if (segIndex >= 0) {
            scheduleRetry(segIndex, tr("Geçersiz HTTP yanıtı: %1").arg(status));
            launchPending();
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
        m_replyLaunchTime.remove(reply);
        m_replyLastActivity.remove(reply);
        m_repliesWithHeaders.remove(reply);

        if (segIndex >= 0) {
            scheduleRetry(segIndex, tr("Geçersiz Content-Range başlığı veya aralık uyuşmazlığı"));
            launchPending();
        }
        return false;
    }

    return true;
}

void SegmentedDownloader::onSegmentReadyRead() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL || !m_segmentReplies.contains(reply)) return;
    if (m_fileFd < 0) return;

    m_repliesWithHeaders.insert(reply);
    m_replyLastActivity[reply] = m_elapsedTimer.elapsed();

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
    m_replyLaunchTime.remove(reply);
    m_replyLastActivity.remove(reply);
    bool hadHeaders = m_repliesWithHeaders.remove(reply);
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    const SegmentInfo &seg = m_item.m_segments.at(segIndex);
    if (reply->error() != QNetworkReply::NoError || !seg.isDone()) {
        if (!hadHeaders && seg.m_received == 0) {
            int cFail = m_connectFailures.value(segIndex, 0) + 1;
            m_connectFailures[segIndex] = cFail;
            if (cFail > s_maxConnectFailures) {
                failDownload(tr("Sunucu yanıt vermiyor (bağlantı zaman aşımı): %1").arg(reply->errorString()));
                return;
            }
        }
        scheduleRetry(segIndex, reply->error() != QNetworkReply::NoError ? reply->errorString() : tr("Segment eksik tamamlandı"));
        launchPending();
        return;
    }

    checkAllSegmentsCompleted();
    launchPending();
}

void SegmentedDownloader::scheduleRetry(int segIndex, const QString &reason) {
    if (m_stopped || m_finished) return;

    int retries = m_segmentRetries.value(segIndex, 0) + 1;
    m_segmentRetries[segIndex] = retries;

    if (retries <= s_maxRetries) {
        int delayMs = 1000 * retries;
        QTimer::singleShot(delayMs, this, [this, segIndex]() {
            if (!m_stopped && !m_finished) {
                if (!m_pendingSegments.contains(segIndex)) {
                    m_pendingSegments.append(segIndex);
                }
                launchPending();
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
    m_singleWithHeaders = false;
    QNetworkRequest req = createRequest(m_item.m_url, rangeHeader);
    m_singleReply = m_nam->get(req);
    qint64 now = m_elapsedTimer.elapsed();
    m_singleLaunchTime = now;
    m_singleLastActivity = now;

    connect(m_singleReply, SIGNAL(metaDataChanged()), this, SLOT(onSingleMetaDataChanged()));
    connect(m_singleReply, SIGNAL(readyRead()), this, SLOT(onSingleReadyRead()));
    connect(m_singleReply, SIGNAL(finished()), this, SLOT(onSingleFinished()));
}

void SegmentedDownloader::onSingleMetaDataChanged() {
    m_singleWithHeaders = true;
    m_singleLastActivity = m_elapsedTimer.elapsed();
}

void SegmentedDownloader::onSingleReadyRead() {
    if (m_singleReply == NULL || m_fileFd < 0) return;

    m_singleWithHeaders = true;
    m_singleLastActivity = m_elapsedTimer.elapsed();

    if (!m_singleValidated) {
        m_singleValidated = true;
        int status = m_singleReply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (status == 401 || status == 403 || status == 404 || status == 410) {
            failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
            return;
        }

        if (status == 200 && m_item.m_receivedBytes > 0) {
            // Server ignored Range request on resume: reset and restart from 0
            m_fullRestarts++;
            if (m_fullRestarts > 3) {
                failDownload(tr("Sunucu aralık isteklerini reddediyor ve baştan başlatma sınırı aşıldı"));
                return;
            }
            if (m_singleReply->hasRawHeader("Content-Length")) {
                m_item.m_totalBytes = m_singleReply->rawHeader("Content-Length").toLongLong();
            } else {
                m_item.m_totalBytes = -1;
            }
            m_item.m_supportsRanges = false;
            m_item.m_receivedBytes = 0;
            if (m_fileFd >= 0) {
                if (::ftruncate(m_fileFd, 0) != 0) {
                    qWarning() << "ftruncate failed:" << errno;
                }
                ::lseek(m_fileFd, 0, SEEK_SET);
            }
        } else if (status == 206) {
            // D7: Validate Content-Range header on single-stream 206
            QString crHeader = QString::fromLatin1(m_singleReply->rawHeader("Content-Range")).trimmed();
            if (!RangePlanner::validateContentRangeHeader(crHeader, m_item.m_receivedBytes, -1, m_item.m_totalBytes)) {
                failDownload(tr("Tek akış yanıtında geçersiz Content-Range başlığı: %1").arg(crHeader));
                return;
            }
        } else if (status != 200) {
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
        if (!m_singleWithHeaders && m_item.m_receivedBytes == 0) {
            m_singleConnectFailures++;
            if (m_singleConnectFailures > s_maxConnectFailures) {
                failDownload(tr("Sunucu yanıt vermiyor (bağlantı zaman aşımı): %1").arg(reply->errorString()));
                return;
            }
        }
        if (!m_item.m_supportsRanges) {
            m_fullRestarts++;
            if (m_fullRestarts > 3) {
                failDownload(tr("Tek akış indirmesi baştan başlatma sınırı aşıldı (sınır 3)"));
                return;
            }
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
        if (!m_item.m_supportsRanges) {
            m_fullRestarts++;
            if (m_fullRestarts > 3) {
                failDownload(tr("Tek akış indirmesi baştan başlatma sınırı aşıldı (sınır 3)"));
                return;
            }
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
    m_item.m_finalPath = uniqueTargetPath(m_item.m_directory, m_item.m_fileName, m_item.m_id);
    m_item.m_fileName = QFileInfo(m_item.m_finalPath).fileName();
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
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (QFile::rename(partPath, m_item.m_finalPath)) {
                renameOk = true;
                break;
            }
            applyUniqueFilename();
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

    m_pendingSegments.clear();
    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    m_segmentReplies.clear();
    m_replyExpectedStart.clear();
    m_validatedReplies.clear();
    m_replyLaunchTime.clear();
    m_replyLastActivity.clear();
    m_repliesWithHeaders.clear();
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

    // Monotonic stall and connect timeout detection
    const qint64 stallThresholdMs = 30000;
    const qint64 connectThresholdMs = 60000;

    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply == NULL) continue;
        if (m_repliesWithHeaders.contains(reply)) {
            if (now - m_replyLastActivity.value(reply, now) > stallThresholdMs) {
                qWarning() << "Segment reply stalled for >30s, aborting to trigger retry";
                reply->abort();
            }
        } else {
            if (now - m_replyLaunchTime.value(reply, now) > connectThresholdMs) {
                qWarning() << "Segment reply connect timed out for >60s, aborting";
                reply->abort();
            }
        }
    }

    if (m_singleReply != NULL) {
        if (m_singleWithHeaders) {
            if (now - m_singleLastActivity > stallThresholdMs) {
                qWarning() << "Single stream reply stalled for >30s, aborting to trigger retry";
                m_singleReply->abort();
            }
        } else {
            if (now - m_singleLaunchTime > connectThresholdMs) {
                qWarning() << "Single stream connect timed out for >60s, aborting";
                m_singleReply->abort();
            }
        }
    }

    if (m_probeReply != NULL) {
        if (now - m_probeLaunchTime > 15000) {
            qWarning() << "Probe reply stalled for >15s, aborting to trigger retry";
            m_probeReply->abort();
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
