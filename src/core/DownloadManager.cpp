#include "DownloadManager.h"
#include "MediaHosts.h"
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include <QDateTime>
#include <QTimer>
#include <QCoreApplication>
#include <QSettings>
#include <QDebug>
#include <sys/stat.h>

DownloadManager& DownloadManager::instance() {
    static DownloadManager s_instance;
    return s_instance;
}

DownloadManager::DownloadManager(QObject *parent)
    : QObject(parent),
      m_saveTimer(NULL),
      m_isShuttingDown(false),
      m_maxConcurrent(3),
      m_defaultSegments(8),
      m_serverPort(48213),
      m_clipboardMonitoring(true) {
    m_defaultDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    m_captureExtensions << "zip" << "rar" << "7z" << "tar" << "gz" << "bz2" << "xz" << "tgz"
                        << "deb" << "rpm" << "appimage" << "iso" << "dmg" << "exe" << "msi"
                        << "apk" << "bin" << "mp4" << "mkv" << "avi" << "mov" << "webm"
                        << "mp3" << "wav" << "flac" << "aac" << "m4a" << "pdf" << "doc" << "docx";

    m_saveTimer = new QTimer(this);
    m_saveTimer->setInterval(5000);
    connect(m_saveTimer, SIGNAL(timeout()), this, SLOT(onPeriodicSaveTimeout()));

    if (QCoreApplication::instance()) {
        connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()), this, SLOT(shutdown()));
        connect(QCoreApplication::instance(), SIGNAL(aboutToQuit()), this, SLOT(saveState()));
    }

    loadState();
    initialize();
}

DownloadManager::~DownloadManager() {
    shutdown();
}

void DownloadManager::initialize() {
    QSettings settings("HasanBucak", "hb-download-manager");
    m_defaultDir = settings.value("defaultDirectory", m_defaultDir).toString();
    m_defaultSegments = qBound(1, settings.value("defaultSegments", m_defaultSegments).toInt(), 16);
    m_maxConcurrent = qMax(1, settings.value("maxConcurrentDownloads", m_maxConcurrent).toInt());
    m_serverPort = static_cast<quint16>(settings.value("serverPort", m_serverPort).toUInt());
    m_clipboardMonitoring = settings.value("clipboardMonitoring", m_clipboardMonitoring).toBool();
    processQueue();
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
    if (copy.m_kind == DownloadKindUnspecified) {
        if (MediaHosts::isStreamingSite(copy.m_url.host())) {
            copy.m_kind = DownloadKindVideo;
        } else {
            copy.m_kind = DownloadKindFile;
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

void DownloadManager::stopDownloader(const QString &id, bool immediateDelete) {
    if (m_activeSegmented.contains(id)) {
        SegmentedDownloader *downloader = m_activeSegmented.take(id);
        downloader->disconnect(this);
        downloader->syncToDisk();
        downloader->pause();
        if (immediateDelete) {
            delete downloader;
        } else {
            downloader->deleteLater();
        }
    }
    if (m_activeVideo.contains(id)) {
        VideoDownloader *downloader = m_activeVideo.take(id);
        downloader->disconnect(this);
        downloader->pause();
        if (immediateDelete) {
            delete downloader;
        } else {
            downloader->deleteLater();
        }
    }
}

void DownloadManager::pauseDownload(const QString &id) {
    stopDownloader(id);

    DownloadItem *item = findItem(id);
    if (item != NULL) {
        if (item->m_state == DownloadStateQueued ||
            item->m_state == DownloadStateConnecting ||
            item->m_state == DownloadStateDownloading) {
            item->m_state = DownloadStatePaused;
            item->m_speed = 0.0;
            emit atItemUpdated(*item);
            saveState();
        }
    }

    if (activeCount() == 0 && m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
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

void DownloadManager::pauseAll(bool immediateDelete) {
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items[i].m_state == DownloadStateQueued) {
            m_items[i].m_state = DownloadStatePaused;
            m_items[i].m_speed = 0.0;
            emit atItemUpdated(m_items[i]);
        }
    }

    QList<QString> segKeys = m_activeSegmented.keys();
    for (int i = 0; i < segKeys.size(); ++i) {
        const QString &id = segKeys.at(i);
        stopDownloader(id, immediateDelete);
        DownloadItem *item = findItem(id);
        if (item != NULL) {
            item->m_state = DownloadStatePaused;
            item->m_speed = 0.0;
            emit atItemUpdated(*item);
        }
    }

    QList<QString> vidKeys = m_activeVideo.keys();
    for (int i = 0; i < vidKeys.size(); ++i) {
        const QString &id = vidKeys.at(i);
        stopDownloader(id, immediateDelete);
        DownloadItem *item = findItem(id);
        if (item != NULL) {
            item->m_state = DownloadStatePaused;
            item->m_speed = 0.0;
            emit atItemUpdated(*item);
        }
    }

    if (m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
    }

    saveState();
}

void DownloadManager::shutdown() {
    if (m_isShuttingDown) return;
    m_isShuttingDown = true;

    if (m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
    }

    pauseAll(true);
    saveState();

    for (QMap<QString, SegmentedDownloader*>::iterator it = m_activeSegmented.begin(); it != m_activeSegmented.end(); ++it) {
        SegmentedDownloader *dl = it.value();
        if (dl != NULL) {
            dl->disconnect(this);
            dl->pause();
            delete dl;
        }
    }
    m_activeSegmented.clear();

    for (QMap<QString, VideoDownloader*>::iterator it = m_activeVideo.begin(); it != m_activeVideo.end(); ++it) {
        VideoDownloader *dl = it.value();
        if (dl != NULL) {
            dl->disconnect(this);
            dl->pause();
            delete dl;
        }
    }
    m_activeVideo.clear();
}

void DownloadManager::cancelDownload(const QString &id) {
    DownloadItem *item = findItem(id);
    if (item == NULL) return;
    if (item->m_state == DownloadStateCompleted) return;
    if (item->m_state == DownloadStateCancelled) return;

    stopDownloader(id);
    item->m_state = DownloadStateCancelled;
    item->m_speed = 0.0;
    item->m_cookies = QJsonArray();
    item->m_cookieHeader.clear();
    emit atItemUpdated(*item);
    saveState();

    if (activeCount() == 0 && m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
    }
    processQueue();
}

void DownloadManager::removeDownload(const QString &id, bool deleteFile) {
    DownloadItem *found = findItem(id);
    if (found == NULL) return;

    // F7: Read item state BEFORE stopping downloader (do not turn Completed into Paused)
    const bool wasCompleted = (found->m_state == DownloadStateCompleted);
    const bool wasCancelled = (found->m_state == DownloadStateCancelled);
    const QString finalPath = found->m_finalPath;
    const QString partPath = found->partFilePath();

    stopDownloader(id);

    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_id == id) {
            m_items.removeAt(i);
            break;
        }
    }

    // D1: Delete the .part without asking only for Cancelled items, or if deleteFile is explicitly requested.
    // Keep .part intact for Paused and Error items if deleteFile is false.
    if (wasCancelled || deleteFile) {
        if (QFile::exists(partPath)) {
            QFile::remove(partPath);
        }
    }

    // If deleteFile is true, ONLY remove finalPath if Completed and !finalPath.isEmpty()
    // Never delete targetFilePath() of an incomplete download!
    if (deleteFile && wasCompleted && !finalPath.isEmpty()) {
        if (QFile::exists(finalPath)) {
            QFile::remove(finalPath);
        }
    }

    emit atItemRemoved(id);
    saveState();

    if (activeCount() == 0 && m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
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

    if (m_saveTimer && !m_saveTimer->isActive()) {
        m_saveTimer->start(5000);
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
        it->m_speed = speed;
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
        // S4: Clear sensitive session cookies upon completion or permanent failure
        it->m_cookies = QJsonArray();
        it->m_cookieHeader.clear();

        if (success) {
            emit atItemUpdated(*it);
            emit atItemCompleted(*it);
        } else {
            emit atItemUpdated(*it);
            emit atItemFailed(*it, errMsg);
        }
    }

    if (activeCount() == 0 && m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
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
                it->m_speed = currentSpeed;
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
        // S4: Clear sensitive session cookies upon completion or permanent failure
        it->m_cookies = QJsonArray();
        it->m_cookieHeader.clear();

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

    if (activeCount() == 0 && m_saveTimer && m_saveTimer->isActive()) {
        m_saveTimer->stop();
    }

    saveState();
    processQueue();
}

void DownloadManager::onPeriodicSaveTimeout() {
    if (activeCount() > 0) {
        // D5: Sync active segmented downloaders to disk (fdatasync) before taking snapshot
        for (QMap<QString, SegmentedDownloader*>::iterator it = m_activeSegmented.begin(); it != m_activeSegmented.end(); ++it) {
            if (it.value() != NULL) {
                it.value()->syncToDisk();
            }
        }
        saveState();
    } else {
        if (m_saveTimer && m_saveTimer->isActive()) {
            m_saveTimer->stop();
        }
    }
}

QString DownloadManager::configFilePath() const {
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/hb-download-manager";
    QDir().mkpath(configDir);
    ::chmod(configDir.toLocal8Bit().constData(), 0700);
    return configDir + "/downloads.json";
}

void DownloadManager::saveState() {
    QJsonArray array;
    for (int i = 0; i < m_items.size(); ++i) {
        array.append(m_items.at(i).toJson());
    }

    QJsonDocument doc(array);
    QString filePath = configFilePath();
    QSaveFile saveFile(filePath);
    if (saveFile.open(QIODevice::WriteOnly)) {
        saveFile.write(doc.toJson());
        if (saveFile.commit()) {
            QFile::setPermissions(filePath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        } else {
            qWarning() << "Failed to commit state file:" << filePath;
        }
    } else {
        qWarning() << "Failed to open QSaveFile for writing:" << filePath << saveFile.errorString();
    }
}

void DownloadManager::loadState() {
    QString path = configFilePath();
    QFile file(path);
    if (!file.exists()) return;
    if (!file.open(QIODevice::ReadOnly)) return;

    QByteArray data = file.readAll();
    file.close();

    if (data.trimmed().isEmpty()) return;

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isArray()) {
        qWarning() << "Failed to parse state file:" << parseError.errorString();

        // D7: Clean up corrupt state files older than 7 days
        QDir cfgDir(QFileInfo(path).absolutePath());
        QStringList oldCorrupt = cfgDir.entryList(QStringList() << "downloads.json.corrupt-*", QDir::Files);
        QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < oldCorrupt.size(); ++i) {
            QFileInfo cInfo(cfgDir.filePath(oldCorrupt.at(i)));
            if (cInfo.lastModified().daysTo(now) > 7) {
                cfgDir.remove(oldCorrupt.at(i));
            }
        }

        QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
        QString corruptPath = QString("%1.corrupt-%2").arg(path, timestamp);
        if (QFile::exists(corruptPath)) {
            corruptPath = QString("%1.corrupt-%2").arg(path, QString::number(QDateTime::currentMSecsSinceEpoch()));
        }
        QFile::rename(path, corruptPath);
        QFile::setPermissions(corruptPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        return;
    }

    m_items.clear();
    QJsonArray array = doc.array();
    for (int i = 0; i < array.size(); ++i) {
        m_items.append(DownloadItem::fromJson(array.at(i).toObject()));
    }
}

QString DownloadManager::defaultDirectory() const { return m_defaultDir; }
void DownloadManager::setDefaultDirectory(const QString &dir) {
    m_defaultDir = dir;
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("defaultDirectory", dir);
}

int DownloadManager::defaultSegments() const { return m_defaultSegments; }
void DownloadManager::setDefaultSegments(int count) {
    m_defaultSegments = qBound(1, count, 16);
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("defaultSegments", m_defaultSegments);
}

int DownloadManager::maxConcurrentDownloads() const { return m_maxConcurrent; }
void DownloadManager::setMaxConcurrentDownloads(int count) {
    m_maxConcurrent = qMax(1, count);
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("maxConcurrentDownloads", m_maxConcurrent);
}

quint16 DownloadManager::serverPort() const { return m_serverPort; }
void DownloadManager::setServerPort(quint16 port) {
    m_serverPort = port;
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("serverPort", port);
}

bool DownloadManager::clipboardMonitoringEnabled() const { return m_clipboardMonitoring; }
void DownloadManager::setClipboardMonitoringEnabled(bool enabled) {
    m_clipboardMonitoring = enabled;
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("clipboardMonitoring", enabled);
}

QStringList DownloadManager::captureExtensions() const { return m_captureExtensions; }
