#include "SegmentedDownloader.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrlQuery>

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

SegmentedDownloader::SegmentedDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent),
      m_item(item),
      m_nam(new QNetworkAccessManager(this)),
      m_probeReply(NULL),
      m_singleReply(NULL),
      m_fileFd(-1),
      m_stopped(false),
      m_finished(false),
      m_speedTimer(new QTimer(this)),
      m_retryTimer(new QTimer(this)),
      m_bytesInInterval(0),
      m_lastReportTime(0) {
    connect(m_speedTimer, SIGNAL(timeout()), this, SLOT(onSpeedTimerTimeout()));
    connect(m_retryTimer, SIGNAL(timeout()), this, SLOT(onRetryTimerTimeout()));
    m_retryTimer->setSingleShot(true);
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
            failDownload(tr("Parça dosyası açılamadı"));
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
    m_retryTimer->stop();

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
    connect(m_probeReply, SIGNAL(finished()), this, SLOT(onProbeFinished()));
}

void SegmentedDownloader::onProbeFinished() {
    if (m_probeReply == NULL) return;
    QNetworkReply *reply = m_probeReply;
    m_probeReply = NULL;
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    if (reply->error() != QNetworkReply::NoError) {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 416 || status == 400 || status == 403) {
            m_item.m_supportsRanges = false;
            m_item.m_totalBytes = -1;
            if (openPartFile(false)) {
                startSingleStream();
                return;
            }
        }
        failDownload(reply->errorString());
        return;
    }

    handleProbeReply(reply);
}

void SegmentedDownloader::handleProbeReply(QNetworkReply *reply) {
    int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
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
        QString contentRange = QString::fromUtf8(reply->rawHeader("Content-Range"));
        int slashIdx = contentRange.lastIndexOf('/');
        if (slashIdx != -1) {
            QString lenStr = contentRange.mid(slashIdx + 1).trimmed();
            if (lenStr != "*") {
                total = lenStr.toLongLong();
                ranges = true;
            }
        }
    } else if (statusCode == 200) {
        QString acceptRanges = QString::fromUtf8(reply->rawHeader("Accept-Ranges")).toLower();
        if (reply->hasRawHeader("Content-Length")) {
            total = QString::fromUtf8(reply->rawHeader("Content-Length")).toLongLong();
            ranges = (total > 0 && acceptRanges.contains("bytes"));
        }
    }

    QString contentDisp = QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    QString suggestedName = parseContentDisposition(contentDisp);
    if (suggestedName.isEmpty()) {
        suggestedName = QFileInfo(m_item.m_url.path()).fileName();
    }
    if (suggestedName.isEmpty()) {
        suggestedName = "download";
    }

    if (reply->url().isValid()) {
        m_item.m_url = reply->url();
    }

    if (m_item.m_fileName.isEmpty() || m_item.m_fileName == "download") {
        m_item.m_fileName = suggestedName;
    }
    m_item.m_totalBytes = total;
    m_item.m_supportsRanges = ranges;

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
            failDownload(tr("Part dosyası oluşturulamadı"));
            return;
        }
        m_item.m_state = DownloadStateDownloading;
        startMultiSegment();
    } else {
        if (!openPartFile(false)) {
            failDownload(tr("Part dosyası oluşturulamadı"));
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
    m_fileFd = ::open(partPath.toUtf8().constData(), O_RDWR | O_CREAT, 0664);
    if (m_fileFd < 0) {
        return false;
    }

    if (preallocate && m_item.m_totalBytes > 0) {
        ::posix_fallocate(m_fileFd, 0, m_item.m_totalBytes);
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

    connect(reply, SIGNAL(readyRead()), this, SLOT(onSegmentReadyRead()));
    connect(reply, SIGNAL(finished()), this, SLOT(onSegmentFinished()));
}

void SegmentedDownloader::onSegmentReadyRead() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL || !m_segmentReplies.contains(reply)) return;
    if (m_fileFd < 0) return;

    int segIndex = m_segmentReplies.value(reply);
    SegmentInfo &seg = m_item.m_segments[segIndex];

    QByteArray data = reply->readAll();
    if (data.isEmpty()) return;

    qint64 remaining = seg.length() - seg.m_received;
    if (remaining <= 0) return;

    qint64 toWrite = qMin(static_cast<qint64>(data.size()), remaining);
    off64_t fileOffset = seg.m_start + seg.m_received;

    ssize_t written = ::pwrite64(m_fileFd, data.constData(), toWrite, fileOffset);
    if (written > 0) {
        seg.m_received += written;
        m_item.m_receivedBytes += written;
        m_bytesInInterval += written;
    }
}

void SegmentedDownloader::onSegmentFinished() {
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == NULL) return;

    int segIndex = m_segmentReplies.take(reply);
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    if (reply->error() != QNetworkReply::NoError) {
        int retries = m_segmentRetries.value(segIndex, 0) + 1;
        m_segmentRetries[segIndex] = retries;
        if (retries <= s_maxRetries) {
            m_pendingRetries.append(segIndex);
            m_retryTimer->start(1000 * retries);
            return;
        } else {
            failDownload(tr("Segment %1 indirilemedi: %2").arg(segIndex + 1).arg(reply->errorString()));
            return;
        }
    }

    checkAllSegmentsCompleted();
}

void SegmentedDownloader::onRetryTimerTimeout() {
    if (m_stopped || m_finished) return;
    while (!m_pendingRetries.isEmpty()) {
        int segIndex = m_pendingRetries.takeFirst();
        launchSegment(segIndex);
    }
}

void SegmentedDownloader::startSingleStream() {
    QString rangeHeader;
    if (m_item.m_supportsRanges && m_item.m_receivedBytes > 0) {
        rangeHeader = QString("bytes=%1-").arg(m_item.m_receivedBytes);
    }
    QNetworkRequest req = createRequest(m_item.m_url, rangeHeader);
    m_singleReply = m_nam->get(req);
    connect(m_singleReply, SIGNAL(readyRead()), this, SLOT(onSingleReadyRead()));
    connect(m_singleReply, SIGNAL(finished()), this, SLOT(onSingleFinished()));
}

void SegmentedDownloader::onSingleReadyRead() {
    if (m_singleReply == NULL || m_fileFd < 0) return;

    QByteArray data = m_singleReply->readAll();
    if (data.isEmpty()) return;

    ssize_t written = ::pwrite64(m_fileFd, data.constData(), data.size(), m_item.m_receivedBytes);
    if (written > 0) {
        m_item.m_receivedBytes += written;
        m_bytesInInterval += written;
    }
}

void SegmentedDownloader::onSingleFinished() {
    if (m_singleReply == NULL) return;
    QNetworkReply *reply = m_singleReply;
    m_singleReply = NULL;
    reply->deleteLater();

    if (m_stopped || m_finished) return;

    if (reply->error() != QNetworkReply::NoError) {
        failDownload(reply->errorString());
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
    QString target = QDir(m_item.m_directory).filePath(m_item.m_fileName);
    if (!QFile::exists(target)) {
        m_item.m_finalPath = target;
        return;
    }

    QFileInfo info(target);
    QString baseName = info.completeBaseName();
    QString suffix = info.suffix();
    if (!suffix.isEmpty()) suffix = "." + suffix;

    int counter = 1;
    while (true) {
        QString candidate = QDir(m_item.m_directory).filePath(QString("%1 (%2)%3").arg(baseName).arg(counter).arg(suffix));
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
    m_retryTimer->stop();
    closePartFile();

    applyUniqueFilename();

    QString partPath = m_item.partFilePath();
    if (QFile::exists(partPath)) {
        QFile::rename(partPath, m_item.m_finalPath);
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
    m_retryTimer->stop();
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
    if (elapsedMs <= 0) return;

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
