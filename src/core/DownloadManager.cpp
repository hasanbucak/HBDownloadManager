#include "DownloadManager.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDebug>

DownloadManager& DownloadManager::instance() {
    static DownloadManager s_instance;
    return s_instance;
}

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent),
      m_maxConcurrent(3),
      m_defaultSegments(8),
      m_serverPort(48213),
      m_clipboardMonitoring(true) {
    m_defaultDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    m_captureExtensions << "zip" << "rar" << "7z" << "tar" << "gz" << "bz2" << "xz" << "tgz"
                        << "deb" << "rpm" << "appimage" << "iso" << "dmg" << "exe" << "msi"
                        << "apk" << "bin" << "mp4" << "mkv" << "avi" << "mov" << "webm"
                        << "mp3" << "wav" << "flac" << "aac" << "m4a" << "pdf" << "doc" << "docx";

    loadState();
}

DownloadManager::~DownloadManager() {
    saveState();
}

int DownloadManager::activeCount() const {
    int count = 0;
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).isActive()) count++;
    }
    return count;
}

DownloadItem* DownloadManager::findItem(const QString &id) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_id == id) {
            return &m_items[i];
        }
    }
    return NULL;
}

QString DownloadManager::findIdByDownloader(QObject *downloader) const {
    for (QMap<QString, SegmentedDownloader*>::const_iterator it = m_activeSegmented.constBegin(); it != m_activeSegmented.constEnd(); ++it) {
        if (it.value() == downloader) return it.key();
    }
    for (QMap<QString, VideoDownloader*>::const_iterator it = m_activeVideo.constBegin(); it != m_activeVideo.constEnd(); ++it) {
        if (it.value() == downloader) return it.key();
    }
    return QString();
}

QString DownloadManager::addDownload(const DownloadItem &item, bool startImmediately) {
    DownloadItem copy = item;
    if (copy.m_directory.isEmpty()) {
        copy.m_directory = m_defaultDir;
    }
    if (copy.m_kind == DownloadKindFile) {
        QString host = copy.m_url.host().toLower();
        if (host.contains("youtube.com") || host.contains("youtu.be") ||
            host.contains("vimeo.com") || host.contains("dailymotion.com") ||
            host.contains("tiktok.com") || host.contains("instagram.com") ||
            host.contains("facebook.com") || host.contains("fb.watch") || host.contains("fb.com") ||
            host.contains("twitch.tv") || host.contains("x.com") || host.contains("twitter.com") ||
            !copy.m_quality.isEmpty()) {
            copy.m_kind = DownloadKindVideo;
        }
    }
    if (copy.m_requestedSegments <= 0) {
        copy.m_requestedSegments = m_defaultSegments;
    }

    if (startImmediately) {
        copy.m_state = DownloadStateQueued;
    } else {
        copy.m_state = DownloadStatePaused;
    }

    m_items.prepend(copy);
    emit atItemAdded(copy);
    saveState();

    if (startImmediately) {
        processQueue();
    }

    return copy.m_id;
}

void DownloadManager::pauseDownload(const QString &id) {
    if (m_activeSegmented.contains(id)) {
        SegmentedDownloader *downloader = m_activeSegmented.take(id);
        downloader->disconnect(this);
        downloader->pause();
        downloader->deleteLater();
    }
    if (m_activeVideo.contains(id)) {
        VideoDownloader *downloader = m_activeVideo.take(id);
        downloader->disconnect(this);
        downloader->pause();
        downloader->deleteLater();
    }

    DownloadItem *item = findItem(id);
    if (item != NULL) {
        item->m_state = DownloadStatePaused;
        item->m_speed = 0.0;
        emit atItemUpdated(*item);
        saveState();
    }

    processQueue();
}

void DownloadManager::resumeDownload(const QString &id) {
    DownloadItem *item = findItem(id);
    if (item != NULL && !item->isActive() && item->m_state != DownloadStateCompleted) {
        item->m_state = DownloadStateQueued;
        emit atItemUpdated(*item);
        saveState();
        processQueue();
    }
}

void DownloadManager::cancelDownload(const QString &id) {
    pauseDownload(id);
    DownloadItem *item = findItem(id);
    if (item != NULL) {
        item->m_state = DownloadStateCancelled;
        emit atItemUpdated(*item);
        saveState();
    }
}

void DownloadManager::removeDownload(const QString &id, bool deleteFile) {
    pauseDownload(id);

    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_id == id) {
            DownloadItem item = m_items.at(i);
            m_items.removeAt(i);

            if (deleteFile) {
                if (QFile::exists(item.partFilePath())) {
                    QFile::remove(item.partFilePath());
                }
                if (QFile::exists(item.targetFilePath())) {
                    QFile::remove(item.targetFilePath());
                }
            }

            emit atItemRemoved(id);
            saveState();
            break;
        }
    }

    processQueue();
}

void DownloadManager::processQueue() {
    if (activeCount() >= m_maxConcurrent) return;

    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_state == DownloadStateQueued) {
            startItem(&m_items[i]);
            if (activeCount() >= m_maxConcurrent) break;
        }
    }
}

void DownloadManager::startItem(DownloadItem *item) {
    if (item == NULL) return;
    QString id = item->m_id;

    if (item->m_kind == DownloadKindVideo || item->m_kind == DownloadKindAudio) {
        VideoDownloader *downloader = new VideoDownloader(*item, this);
        m_activeVideo[id] = downloader;
        item->m_state = DownloadStateDownloading;
        emit atItemUpdated(*item);

        connect(downloader, SIGNAL(atTitleDetected(QString)),
                this, SLOT(onVideoTitleDetected(QString)));
        connect(downloader, SIGNAL(atProgressUpdated(double,qint64,qint64,QString,QString,QString)),
                this, SLOT(onVideoProgressUpdated(double,qint64,qint64,QString,QString,QString)));
        connect(downloader, SIGNAL(atFinished(bool,QString,QString)),
                this, SLOT(onVideoFinished(bool,QString,QString)));

        downloader->start();
    } else {
        SegmentedDownloader *downloader = new SegmentedDownloader(*item, this);
        m_activeSegmented[id] = downloader;
        item->m_state = (item->m_segments.isEmpty() ? DownloadStateConnecting : DownloadStateDownloading);
        emit atItemUpdated(*item);

        connect(downloader, SIGNAL(atProbed(ProbeResult)),
                this, SLOT(onSegmentedProbed(ProbeResult)));
        connect(downloader, SIGNAL(atProgressUpdated(qint64,qint64,double,QList<SegmentInfo>)),
                this, SLOT(onSegmentedProgressUpdated(qint64,qint64,double,QList<SegmentInfo>)));
        connect(downloader, SIGNAL(atFinished(bool,QString)),
                this, SLOT(onSegmentedFinished(bool,QString)));

        downloader->start();
    }
}

void DownloadManager::onSegmentedProbed(const ProbeResult &res) {
    QString id = findIdByDownloader(sender());
    DownloadItem *it = findItem(id);
    if (it != NULL) {
        it->m_state = DownloadStateDownloading;
        it->m_totalBytes = res.m_totalBytes;
        it->m_supportsRanges = res.m_supportsRanges;
        if (!res.m_suggestedFileName.isEmpty()) {
            it->m_fileName = res.m_suggestedFileName;
        }
        emit atItemUpdated(*it);
    }
}

void DownloadManager::onSegmentedProgressUpdated(qint64 received, qint64 total, double speed, const QList<SegmentInfo> &segs) {
    QString id = findIdByDownloader(sender());
    DownloadItem *it = findItem(id);
    if (it != NULL) {
        it->m_state = DownloadStateDownloading;
        it->m_receivedBytes = received;
        it->m_totalBytes = total;
        if (it->m_speed <= 0.0) {
            it->m_speed = speed;
        } else {
            it->m_speed = 0.35 * speed + 0.65 * it->m_speed;
        }
        it->m_segments = segs;
        emit atItemUpdated(*it);
    }
}

void DownloadManager::onSegmentedFinished(bool success, const QString &errMsg) {
    SegmentedDownloader *downloader = qobject_cast<SegmentedDownloader*>(sender());
    if (downloader == NULL) return;

    QString id = findIdByDownloader(downloader);
    m_activeSegmented.remove(id);
    DownloadItem updated = downloader->item();
    downloader->deleteLater();

    DownloadItem *it = findItem(id);
    if (it != NULL) {
        *it = updated;
        if (success) {
            emit atItemUpdated(*it);
            emit atItemCompleted(*it);
        } else {
            emit atItemUpdated(*it);
            emit atItemFailed(*it, errMsg);
        }
    }
    saveState();
    processQueue();
}

void DownloadManager::onVideoTitleDetected(const QString &title) {
    QString id = findIdByDownloader(sender());
    DownloadItem *it = findItem(id);
    if (it != NULL && !title.isEmpty()) {
        it->m_fileName = title;
        emit atItemUpdated(*it);
    }
}

static double parseSpeedStringToBytes(const QString &speedStr) {
    QString str = speedStr.trimmed();
    if (str.endsWith("/s", Qt::CaseInsensitive)) {
        QString s = str.left(str.length() - 2).trimmed();
        if (s.endsWith("GiB", Qt::CaseInsensitive) || s.endsWith("GB", Qt::CaseInsensitive)) {
            int cut = s.endsWith("GiB", Qt::CaseInsensitive) ? 3 : 2;
            return s.left(s.length() - cut).trimmed().toDouble() * 1024.0 * 1024.0 * 1024.0;
        }
        if (s.endsWith("MiB", Qt::CaseInsensitive) || s.endsWith("MB", Qt::CaseInsensitive)) {
            int cut = s.endsWith("MiB", Qt::CaseInsensitive) ? 3 : 2;
            return s.left(s.length() - cut).trimmed().toDouble() * 1024.0 * 1024.0;
        }
        if (s.endsWith("KiB", Qt::CaseInsensitive) || s.endsWith("KB", Qt::CaseInsensitive)) {
            int cut = s.endsWith("KiB", Qt::CaseInsensitive) ? 3 : 2;
            return s.left(s.length() - cut).trimmed().toDouble() * 1024.0;
        }
        if (s.endsWith("B", Qt::CaseInsensitive)) {
            return s.left(s.length() - 1).trimmed().toDouble();
        }
    }
    return 0.0;
}

void DownloadManager::onVideoProgressUpdated(double pct, qint64 totalBytes, qint64 receivedBytes, const QString &speedStr, const QString &etaStr, const QString &stage) {
    Q_UNUSED(etaStr);
    QString id = findIdByDownloader(sender());
    DownloadItem *it = findItem(id);
    if (it != NULL) {
        it->m_stage = stage;
        it->m_videoPercent = pct;
        if (totalBytes > 0) {
            it->m_totalBytes = totalBytes;
            it->m_receivedBytes = (receivedBytes > 0 ? receivedBytes : static_cast<qint64>((pct / 100.0) * static_cast<double>(totalBytes)));
        } else if (receivedBytes > 0) {
            it->m_receivedBytes = receivedBytes;
        }
        if (!speedStr.isEmpty() && !speedStr.contains("Unknown", Qt::CaseInsensitive) && speedStr != "NA") {
            double currentSpeed = parseSpeedStringToBytes(speedStr);
            if (currentSpeed > 0.0) {
                if (it->m_speed <= 0.0) {
                    it->m_speed = currentSpeed;
                } else {
                    it->m_speed = 0.35 * currentSpeed + 0.65 * it->m_speed;
                }
            }
        }
        emit atItemUpdated(*it);
    }
}

void DownloadManager::onVideoFinished(bool success, const QString &finalPath, const QString &errMsg) {
    VideoDownloader *downloader = qobject_cast<VideoDownloader*>(sender());
    if (downloader == NULL) return;

    QString id = findIdByDownloader(downloader);
    m_activeVideo.remove(id);
    DownloadItem updated = downloader->item();
    downloader->deleteLater();

    DownloadItem *it = findItem(id);
    if (it != NULL) {
        if (success) {
            it->m_state = DownloadStateCompleted;
            it->m_finalPath = (finalPath.isEmpty() ? updated.m_finalPath : finalPath);
            it->m_videoPercent = 100.0;
            it->m_speed = 0.0;
            if (!it->m_finalPath.isEmpty()) {
                QFileInfo fi(it->m_finalPath);
                it->m_fileName = fi.fileName();
                if (fi.exists()) {
                    it->m_totalBytes = fi.size();
                    it->m_receivedBytes = fi.size();
                }
            }
            if (it->m_totalBytes <= 0 && updated.m_totalBytes > 0) {
                it->m_totalBytes = updated.m_totalBytes;
                it->m_receivedBytes = updated.m_totalBytes;
            }
            it->m_completedAt = QDateTime::currentDateTime();
            emit atItemUpdated(*it);
            emit atItemCompleted(*it);
        } else {
            it->m_state = DownloadStateError;
            it->m_errorMessage = errMsg;
            it->m_speed = 0.0;
            emit atItemUpdated(*it);
            emit atItemFailed(*it, errMsg);
        }
    }
    saveState();
    processQueue();
}

QString DownloadManager::configFilePath() const {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/hb-download-manager";
    QDir().mkpath(configDir);
    return configDir + "/downloads.json";
}

void DownloadManager::saveState() {
    QJsonArray array;
    for (int i = 0; i < m_items.size(); ++i) {
        array.append(m_items.at(i).toJson());
    }

    QJsonDocument doc(array);
    QFile file(configFilePath());
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson());
        file.close();
    }
}

void DownloadManager::loadState() {
    QFile file(configFilePath());
    if (!file.open(QIODevice::ReadOnly)) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isArray()) return;

    m_items.clear();
    QJsonArray array = doc.array();
    for (int i = 0; i < array.size(); ++i) {
        m_items.append(DownloadItem::fromJson(array.at(i).toObject()));
    }
}

QString DownloadManager::defaultDirectory() const { return m_defaultDir; }
void DownloadManager::setDefaultDirectory(const QString &dir) { m_defaultDir = dir; }

int DownloadManager::defaultSegments() const { return m_defaultSegments; }
void DownloadManager::setDefaultSegments(int count) { m_defaultSegments = qBound(1, count, 16); }

int DownloadManager::maxConcurrentDownloads() const { return m_maxConcurrent; }
void DownloadManager::setMaxConcurrentDownloads(int count) { m_maxConcurrent = qMax(1, count); }

quint16 DownloadManager::serverPort() const { return m_serverPort; }
void DownloadManager::setServerPort(quint16 port) { m_serverPort = port; }

bool DownloadManager::clipboardMonitoringEnabled() const { return m_clipboardMonitoring; }
void DownloadManager::setClipboardMonitoringEnabled(bool enabled) { m_clipboardMonitoring = enabled; }

QStringList DownloadManager::captureExtensions() const { return m_captureExtensions; }
