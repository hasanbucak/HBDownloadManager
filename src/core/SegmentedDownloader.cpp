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

qint64 SegmentedDownloader::s_stallMs = 30000;
qint64 SegmentedDownloader::s_connectMs = 60000;
qint64 SegmentedDownloader::s_probeMs = 15000;

void SegmentedDownloader::setTimeoutsForTesting(qint64 stallMs, qint64 connectMs, qint64 probeMs) {
    if (stallMs > 0) s_stallMs = stallMs;
    if (connectMs > 0) s_connectMs = connectMs;
    if (probeMs > 0) s_probeMs = probeMs;
}

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

namespace {
class BridgeCookieJar : public QNetworkCookieJar {
public:
    explicit BridgeCookieJar(QObject *parent = nullptr) : QNetworkCookieJar(parent) {}
    void populate(const QList<QNetworkCookie> &cookies) {
        setAllCookies(cookies);
    }
};

// RFC 7233: only strong validators may be used with If-Range
QString strongEtagOrEmpty(const QByteArray &raw) {
    const QString etag = QString::fromLatin1(raw).trimmed();
    if (etag.isEmpty() || etag.startsWith(QLatin1String("W/"), Qt::CaseInsensitive)) {
        return QString();
    }
    return etag;
}

int httpStatusOf(QNetworkReply *reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool isAuthOrGoneStatus(int status) {
    return status == 401 || status == 403 || status == 410;
}
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
      m_lastReportTime(0),
      m_generation(0),
      m_triedOriginalUrl(false),
      m_keepSegmentsOnProbe(false),
      m_durableSegments(item.m_segments),
      m_durableReceived(item.m_receivedBytes) {
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

void SegmentedDownloader::syncToDisk() {
    if (m_fileFd >= 0 && ::fdatasync(m_fileFd) != 0) {
        // Keep the previous durable snapshot: the newest bytes are not known to be on disk
        qWarning() << "fdatasync failed:" << errno;
        return;
    }
    m_durableSegments = m_item.m_segments;
    m_durableReceived = m_item.m_receivedBytes;
}

DownloadItem SegmentedDownloader::durableItem() const {
    DownloadItem copy = m_item;
    copy.m_segments = m_durableSegments;
    copy.m_receivedBytes = m_durableReceived;
    return copy;
}

void SegmentedDownloader::resetDurable() {
    // Called right after the part file was truncated or re-planned: what is on disk now is
    // exactly what the fresh counters say (nothing received)
    m_durableSegments = m_item.m_segments;
    m_durableReceived = m_item.m_receivedBytes;
}

void SegmentedDownloader::start() {
    m_stopped = false;
    m_finished = false;
    m_elapsedTimer.start();
    m_lastReportTime = 0;
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

void SegmentedDownloader::abortAllReplies() {
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
}

void SegmentedDownloader::pause() {
    if (m_stopped || m_finished) return;
    m_stopped = true;
    m_generation++;
    m_speedTimer->stop();

    abortAllReplies();
    closePartFile();

    m_item.m_state = DownloadStatePaused;
    m_item.m_speed = 0.0;
    emit atProgressUpdated(m_item.m_receivedBytes, m_item.m_totalBytes, 0.0, m_item.m_segments);
}

void SegmentedDownloader::cancel() {
    pause();
    m_item.m_state = DownloadStateCancelled;
}

QNetworkRequest SegmentedDownloader::createRequest(const QUrl &url, const QString &rangeHeader, bool allowIfRange) const {
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(10);
    req.setRawHeader("User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36 HBDownloadManager/1.3");
    req.setRawHeader("Accept-Encoding", "identity");

    if (m_item.m_cookies.isEmpty() && !m_item.m_cookieHeader.isEmpty()) {
        req.setRawHeader("Cookie", m_item.m_cookieHeader.toUtf8());
    }

    for (QMap<QString, QString>::const_iterator it = m_item.m_headers.constBegin(); it != m_item.m_headers.constEnd(); ++it) {
        QString value = it.value();
        if (it.key().compare(QLatin1String("Referer"), Qt::CaseInsensitive) == 0) {
            // Cross-origin: send only the origin (strict-origin-when-cross-origin), as browsers do
            const QUrl ref(value);
            if (ref.isValid() && ref.host().compare(url.host(), Qt::CaseInsensitive) != 0) {
                value = ref.adjusted(QUrl::RemovePath | QUrl::RemoveQuery | QUrl::RemoveFragment | QUrl::RemoveUserInfo).toString() + QLatin1Char('/');
            }
        }
        req.setRawHeader(it.key().toUtf8(), value.toUtf8());
    }

    if (!rangeHeader.isEmpty()) {
        req.setRawHeader("Range", rangeHeader.toUtf8());
        if (allowIfRange && !m_item.m_etag.isEmpty()) {
            req.setRawHeader("If-Range", m_item.m_etag.toUtf8());
        }
    }

    return req;
}

bool SegmentedDownloader::switchToOriginalUrl() {
    if (m_triedOriginalUrl) return false;
    if (!m_item.m_originalUrl.isValid() || m_item.m_originalUrl.isEmpty()) return false;
    if (m_item.m_url == m_item.m_originalUrl) return false;

    m_triedOriginalUrl = true;
    qWarning() << "Effective URL rejected, retrying with the original URL";
    m_item.m_url = m_item.m_originalUrl;
    restartFromProbe(true);
    return true;
}

void SegmentedDownloader::restartFromProbe(bool keepSegments) {
    m_generation++;
    abortAllReplies();
    m_segmentRetries.clear();
    m_connectFailures.clear();

    if (!keepSegments) {
        m_item.m_segments.clear();
        m_item.m_receivedBytes = 0;
        if (m_fileFd >= 0 && ::ftruncate(m_fileFd, 0) != 0) {
            qWarning() << "ftruncate failed:" << errno;
        }
        resetDurable();
    }
    closePartFile();

    m_keepSegmentsOnProbe = keepSegments;
    m_item.m_state = DownloadStateConnecting;
    probe();
}

void SegmentedDownloader::probe() {
    m_probeLaunchTime = m_elapsedTimer.elapsed();
    // No If-Range on the probe: a stale validator would turn the 206 into a 200
    QNetworkRequest req = createRequest(m_item.m_url, "bytes=0-0", false);
    m_probeReply = m_nam->get(req);
    connect(m_probeReply, SIGNAL(metaDataChanged()), this, SLOT(onProbeMetaDataChanged()));
    connect(m_probeReply, SIGNAL(finished()), this, SLOT(onProbeFinished()));
}

void SegmentedDownloader::onProbeMetaDataChanged() {
    if (m_probeReply == NULL) return;
    QNetworkReply *reply = m_probeReply;

    if (m_stopped || m_finished) return;

    int statusCode = httpStatusOf(reply);
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
            const int gen = m_generation;
            QTimer::singleShot(1000 * m_probeRetries, this, [this, gen]() {
                if (gen == m_generation && !m_stopped && !m_finished) {
                    probe();
                }
            });
            return;
        }
        failDownload(tr("Sunucu hatası (HTTP %1)").arg(statusCode));
        return;
    }

    if (isAuthOrGoneStatus(statusCode)) {
        reply->abort();
        reply->deleteLater();
        if (switchToOriginalUrl()) return;
        failDownload(tr("Erişim engellendi veya yetkilendirme gerekli (HTTP %1)").arg(statusCode));
        return;
    }

    if (statusCode == 404) {
        reply->abort();
        reply->deleteLater();
        if (switchToOriginalUrl()) return;
        failDownload(tr("Dosya bulunamadı (HTTP %1)").arg(statusCode));
        return;
    }

    if (statusCode == 416 || statusCode == 501) {
        reply->abort();
        reply->deleteLater();
        m_item.m_supportsRanges = false;
        m_item.m_totalBytes = -1;
        m_item.m_segments.clear();
        if (openPartFile(false)) {
            m_item.m_state = DownloadStateDownloading;
            startSingleStream();
        } else if (!m_finished) {
            failDownload(tr("Part dosyası oluşturulamadı"));
        }
        return;
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
        int status = httpStatusOf(reply);
        if (status == 416 || status == 501) {
            m_item.m_supportsRanges = false;
            m_item.m_totalBytes = -1;
            m_item.m_segments.clear();
            if (openPartFile(false)) {
                m_item.m_state = DownloadStateDownloading;
                startSingleStream();
                return;
            }
        }
        if (isAuthOrGoneStatus(status)) {
            if (switchToOriginalUrl()) return;
            failDownload(tr("Erişim engellendi veya yetkilendirme gerekli (HTTP %1)").arg(status));
            return;
        }
        if (status == 404) {
            if (switchToOriginalUrl()) return;
            failDownload(tr("Dosya bulunamadı (HTTP %1)").arg(status));
            return;
        }

        // Retry transient network errors or 5xx up to 3 times
        if (m_probeRetries < 3) {
            m_probeRetries++;
            if (m_item.m_originalUrl.isValid() && m_item.m_url != m_item.m_originalUrl) {
                m_item.m_url = m_item.m_originalUrl;
            }
            const int gen = m_generation;
            QTimer::singleShot(1000 * m_probeRetries, this, [this, gen]() {
                if (gen == m_generation && !m_stopped && !m_finished) {
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
    int statusCode = httpStatusOf(reply);
    if (statusCode < 200 || statusCode >= 300) {
        failDownload(tr("Sunucu hatası (HTTP %1)").arg(statusCode));
        return;
    }

    const QString probeEtag = strongEtagOrEmpty(reply->rawHeader("ETag"));

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

    // Server's name first, then the name at the end of the redirect chain
    QString contentDisp = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    QString suggestedName = parseContentDisposition(contentDisp);
    if (suggestedName.isEmpty() && reply->url().isValid()) {
        suggestedName = QFileInfo(reply->url().path()).fileName();
    }
    if (suggestedName.isEmpty()) {
        suggestedName = QFileInfo(m_item.m_url.path()).fileName();
    }
    suggestedName = PathUtils::sanitizeFileName(suggestedName);

    if (reply->url().isValid()) {
        m_item.m_url = reply->url();
    }

    // A name the user or the browser chose stays; a URL-derived guess gives way to the server's.
    // The decision is made once: a later re-probe (resume) must not rename the download.
    if (m_item.m_fileName.isEmpty() || m_item.m_fileName == "download"
        || (!m_item.m_fileNameExplicit && !suggestedName.isEmpty())) {
        if (!suggestedName.isEmpty()) {
            m_item.m_fileName = suggestedName;
        }
    }
    m_item.m_fileNameExplicit = true;

    // Check for HTML response when expecting a non-HTML file
    QString contentType = QString::fromUtf8(reply->rawHeader("Content-Type")).toLower();
    QString lowerName = m_item.m_fileName.toLower();
    if (contentType.startsWith("text/html") && !lowerName.endsWith(".html") && !lowerName.endsWith(".htm")) {
        failDownload(tr("Bağlantı süresi dolmuş veya oturum açma sayfası döndü (Content-Type: text/html)"));
        return;
    }

    // Re-probe after switching to the original URL: keep the existing progress when the
    // remote file is evidently the same one
    const bool keepSegments = m_keepSegmentsOnProbe;
    m_keepSegmentsOnProbe = false;
    if (keepSegments && ranges && total > 0 && total == m_item.m_totalBytes
        && !m_item.m_segments.isEmpty()
        && (m_item.m_etag.isEmpty() || probeEtag.isEmpty() || probeEtag == m_item.m_etag)
        && QFileInfo(m_item.partFilePath()).size() == total) {
        if (!probeEtag.isEmpty()) {
            m_item.m_etag = probeEtag;
        }
        m_item.m_supportsRanges = true;

        ProbeResult res;
        res.m_totalBytes = total;
        res.m_supportsRanges = true;
        res.m_suggestedFileName = m_item.m_fileName;
        res.m_contentType = QString::fromUtf8(reply->rawHeader("Content-Type"));
        res.m_finalUrl = reply->url();
        res.m_etag = m_item.m_etag;
        emit atProbed(res);

        if (!openPartFile(false)) {
            if (!m_finished) {
                failDownload(tr("Parça dosyası açılamadı"));
            }
            return;
        }
        m_item.m_state = DownloadStateDownloading;
        startMultiSegment();
        return;
    }

    m_item.m_etag = probeEtag;
    m_item.m_totalBytes = total;
    m_item.m_supportsRanges = ranges;

    ProbeResult res;
    res.m_totalBytes = total;
    res.m_supportsRanges = ranges;
    res.m_suggestedFileName = m_item.m_fileName;
    res.m_contentType = QString::fromUtf8(reply->rawHeader("Content-Type"));
    res.m_finalUrl = reply->url();
    res.m_etag = probeEtag;
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
        resetDurable();
        m_item.m_state = DownloadStateDownloading;
        startMultiSegment();
    } else {
        m_item.m_segments.clear();
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
    closePartFile();

    QDir dir(m_item.m_directory);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    QString partPath = m_item.partFilePath();
    m_fileFd = ::open(partPath.toUtf8().constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (m_fileFd < 0) {
        qWarning() << "open part file failed:" << strerror(errno);
        return false;
    }

    if (preallocate && m_item.m_totalBytes > 0) {
        if (::ftruncate(m_fileFd, m_item.m_totalBytes) != 0) {
            qWarning() << "ftruncate before fallocate failed:" << errno;
        }
        int ret = ::posix_fallocate(m_fileFd, 0, m_item.m_totalBytes);
        if (ret != 0) {
            if (ret == ENOSPC || ret == EFBIG) {
                // Shrink back: a full-size file would let the next resume skip the space check
                if (::ftruncate(m_fileFd, 0) != 0) {
                    qWarning() << "ftruncate after fallocate failure failed:" << errno;
                }
                ::close(m_fileFd);
                m_fileFd = -1;
                failDownload(ret == ENOSPC ? tr("Yetersiz disk alanı")
                                           : tr("Dosya çok büyük (dosya sistemi sınırı)"));
                return false;
            }
            qWarning() << "posix_fallocate returned error:" << ret;
        }
    }

    return true;
}

void SegmentedDownloader::closePartFile() {
    if (m_fileFd >= 0) {
        // Make the counters durable before the fd goes away
        if (::fdatasync(m_fileFd) == 0) {
            m_durableSegments = m_item.m_segments;
            m_durableReceived = m_item.m_receivedBytes;
        } else {
            qWarning() << "fdatasync on close failed:" << errno;
        }
        ::close(m_fileFd);
        m_fileFd = -1;
    }
}

void SegmentedDownloader::startMultiSegment() {
    if (m_item.m_segments.isEmpty()) {
        // Nothing planned (corrupt state): plan again from a fresh probe
        restartFromProbe(false);
        return;
    }

    m_pendingSegments.clear();
    for (int i = 0; i < m_item.m_segments.size(); ++i) {
        if (!m_item.m_segments.at(i).isDone()) {
            m_pendingSegments.append(i);
        }
    }

    if (m_pendingSegments.isEmpty() && m_segmentReplies.isEmpty()) {
        // All data is already on disk (resume after a late pause/crash or rename failure)
        checkAllSegmentsCompleted();
        return;
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
    if (index < 0 || index >= m_item.m_segments.size()) return;
    const SegmentInfo &seg = m_item.m_segments.at(index);
    qint64 currentOffset = seg.m_start + seg.m_received;
    if (currentOffset > seg.m_end) return;

    QString rangeStr = QString("bytes=%1-%2").arg(currentOffset).arg(seg.m_end);
    QNetworkRequest req = createRequest(m_item.m_url, rangeStr, true);

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

void SegmentedDownloader::fallBackToSingleStream() {
    qWarning() << "Server returned HTTP 200 on Range request. Falling back to single stream.";
    m_generation++;
    abortAllReplies();
    m_segmentRetries.clear();
    m_connectFailures.clear();

    m_item.m_supportsRanges = false;
    m_item.m_segments.clear();
    m_item.m_receivedBytes = 0;
    if (m_fileFd >= 0) {
        if (::ftruncate(m_fileFd, 0) != 0) {
            qWarning() << "ftruncate failed:" << errno;
        }
        ::lseek(m_fileFd, 0, SEEK_SET);
    }
    resetDurable();
    startSingleStream();
}

bool SegmentedDownloader::validateSegmentReply(QNetworkReply *reply, const SegmentInfo &seg) {
    if (reply == NULL) return false;

    int status = httpStatusOf(reply);

    if (status == 200) {
        const QString replyEtag = strongEtagOrEmpty(reply->rawHeader("ETag"));
        if (!m_item.m_etag.isEmpty() && replyEtag != m_item.m_etag) {
            // If-Range was sent and the validator differs: the remote file changed. Start over cleanly.
            m_fullRestarts++;
            if (m_fullRestarts > 3) {
                failDownload(tr("Uzak dosya sürekli değişiyor; baştan başlatma sınırı aşıldı"));
                return false;
            }
            qWarning() << "If-Range mismatch: remote file changed, restarting from a fresh probe";
            m_item.m_etag.clear();
            restartFromProbe(false);
            return false;
        }
        // The server ignored the Range header: fall back to a single stream from byte 0
        fallBackToSingleStream();
        return false;
    }

    if (isAuthOrGoneStatus(status) || status == 404) {
        if (switchToOriginalUrl()) return false;
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
    if (segIndex < 0 || segIndex >= m_item.m_segments.size()) return;
    SegmentInfo &seg = m_item.m_segments[segIndex];

    if (!m_validatedReplies.contains(reply)) {
        if (!validateSegmentReply(reply, seg)) {
            return;  // seg may be gone now (fallback/restart): do not touch it
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

    // Segment made progress: reset retry counters
    m_segmentRetries[segIndex] = 0;
    m_connectFailures[segIndex] = 0;
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
    if (segIndex < 0 || segIndex >= m_item.m_segments.size()) {
        launchPending();
        return;
    }

    const SegmentInfo &seg = m_item.m_segments.at(segIndex);
    if (seg.isDone()) {
        // All bytes of this segment are written; a late transport error does not matter
        checkAllSegmentsCompleted();
        launchPending();
        return;
    }

    // An error status whose body never reached readyRead (e.g. empty 403)
    const int status = httpStatusOf(reply);
    if (isAuthOrGoneStatus(status) || status == 404) {
        if (switchToOriginalUrl()) return;
        failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
        return;
    }

    const QString reason = reply->error() != QNetworkReply::NoError ? reply->errorString() : tr("Segment eksik tamamlandı");
    if (!hadHeaders) {
        // Never got a response: count only against the connect budget
        int cFail = m_connectFailures.value(segIndex, 0) + 1;
        m_connectFailures[segIndex] = cFail;
        if (cFail > s_maxConnectFailures) {
            failDownload(tr("Sunucu yanıt vermiyor (bağlantı zaman aşımı): %1").arg(reason));
            return;
        }
        scheduleRetry(segIndex, reason, false);
    } else {
        scheduleRetry(segIndex, reason, true);
    }
    launchPending();
}

void SegmentedDownloader::scheduleRetry(int segIndex, const QString &reason, bool countAsRetry) {
    if (m_stopped || m_finished) return;

    int delayMs = 1000;
    if (countAsRetry) {
        int retries = m_segmentRetries.value(segIndex, 0) + 1;
        m_segmentRetries[segIndex] = retries;
        if (retries > s_maxRetries) {
            failDownload(tr("Segment %1 indirilemedi (%2 deneme): %3")
                .arg(segIndex + 1).arg(s_maxRetries).arg(reason));
            return;
        }
        delayMs = 1000 * retries;
    } else {
        delayMs = 1000 * qBound(1, m_connectFailures.value(segIndex, 1), 5);
    }

    const int gen = m_generation;
    QTimer::singleShot(delayMs, this, [this, segIndex, gen]() {
        if (gen != m_generation || m_stopped || m_finished) return;
        if (segIndex < 0 || segIndex >= m_item.m_segments.size()) return;
        if (m_item.m_segments.at(segIndex).isDone()) {
            checkAllSegmentsCompleted();
            return;
        }
        if (!m_pendingSegments.contains(segIndex)) {
            m_pendingSegments.append(segIndex);
        }
        launchPending();
    });
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
        resetDurable();
    }

    m_singleValidated = false;
    m_singleWithHeaders = false;
    QNetworkRequest req = createRequest(m_item.m_url, rangeHeader, true);
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
        int status = httpStatusOf(m_singleReply);

        if (isAuthOrGoneStatus(status) || status == 404) {
            if (switchToOriginalUrl()) return;
            failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
            return;
        }

        if (status == 200) {
            if (m_item.m_receivedBytes > 0) {
                // Server ignored Range request on resume: reset and restart from 0
                m_fullRestarts++;
                if (m_fullRestarts > 3) {
                    failDownload(tr("Sunucu aralık isteklerini reddediyor ve baştan başlatma sınırı aşıldı"));
                    return;
                }
                m_item.m_receivedBytes = 0;
                if (m_fileFd >= 0) {
                    if (::ftruncate(m_fileFd, 0) != 0) {
                        qWarning() << "ftruncate failed:" << errno;
                    }
                    ::lseek(m_fileFd, 0, SEEK_SET);
                }
                resetDurable();
            }
            // A full body: its Content-Length is the size now, whatever an earlier probe said
            if (m_singleReply->hasRawHeader("Content-Length")) {
                m_item.m_totalBytes = m_singleReply->rawHeader("Content-Length").toLongLong();
            } else {
                m_item.m_totalBytes = -1;
            }
            m_item.m_supportsRanges = false;
        } else if (status == 206) {
            // D7: Validate Content-Range header on single-stream 206
            QString crHeader = QString::fromLatin1(m_singleReply->rawHeader("Content-Range")).trimmed();
            if (!RangePlanner::validateContentRangeHeader(crHeader, m_item.m_receivedBytes, -1, m_item.m_totalBytes)) {
                failDownload(tr("Tek akış yanıtında geçersiz Content-Range başlığı: %1").arg(crHeader));
                return;
            }
        } else {
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

    const int status = httpStatusOf(reply);
    if (isAuthOrGoneStatus(status) || status == 404) {
        if (switchToOriginalUrl()) return;
        failDownload(tr("Erişim reddedildi veya dosya bulunamadı (HTTP %1)").arg(status));
        return;
    }

    const bool incomplete = (reply->error() != QNetworkReply::NoError)
                            || (m_item.m_totalBytes > 0 && m_item.m_receivedBytes < m_item.m_totalBytes);
    if (incomplete) {
        if (reply->error() != QNetworkReply::NoError && !m_singleWithHeaders && m_item.m_receivedBytes == 0) {
            m_singleConnectFailures++;
            if (m_singleConnectFailures > s_maxConnectFailures) {
                failDownload(tr("Sunucu yanıt vermiyor (bağlantı zaman aşımı): %1").arg(reply->errorString()));
                return;
            }
        }
        if (!m_item.m_supportsRanges) {
            // The next attempt starts from byte 0 again: bounded separately
            m_fullRestarts++;
            if (m_fullRestarts > 3) {
                failDownload(tr("Tek akış indirmesi baştan başlatma sınırı aşıldı (sınır 3)"));
                return;
            }
        }
        m_singleRetries++;
        if (m_singleRetries > s_maxRetries) {
            failDownload(reply->error() != QNetworkReply::NoError ? reply->errorString() : tr("İndirme eksik tamamlandı"));
            return;
        }
        const int gen = m_generation;
        QTimer::singleShot(1000 * m_singleRetries, this, [this, gen]() {
            if (gen == m_generation && !m_stopped && !m_finished) {
                startSingleStream();
            }
        });
        return;
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
    m_generation++;
    m_speedTimer->stop();
    abortAllReplies();
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
    m_generation++;
    m_speedTimer->stop();

    abortAllReplies();
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

    // Monotonic stall and connect timeout detection. abort() may emit finished() synchronously,
    // which edits the maps: iterate over a copy and re-check membership.
    QList<QNetworkReply*> replies = m_segmentReplies.keys();
    for (int i = 0; i < replies.size(); ++i) {
        QNetworkReply *reply = replies.at(i);
        if (reply == NULL || !m_segmentReplies.contains(reply)) continue;
        if (m_repliesWithHeaders.contains(reply)) {
            if (now - m_replyLastActivity.value(reply, now) > s_stallMs) {
                qWarning() << "Segment reply stalled, aborting to trigger retry";
                reply->abort();
            }
        } else {
            if (now - m_replyLaunchTime.value(reply, now) > s_connectMs) {
                qWarning() << "Segment reply connect timed out, aborting";
                reply->abort();
            }
        }
    }

    if (m_singleReply != NULL) {
        if (m_singleWithHeaders) {
            if (now - m_singleLastActivity > s_stallMs) {
                qWarning() << "Single stream reply stalled, aborting to trigger retry";
                m_singleReply->abort();
            }
        } else {
            if (now - m_singleLaunchTime > s_connectMs) {
                qWarning() << "Single stream connect timed out, aborting";
                m_singleReply->abort();
            }
        }
    }

    if (m_probeReply != NULL) {
        if (now - m_probeLaunchTime > s_probeMs) {
            qWarning() << "Probe reply stalled, aborting to trigger retry";
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
