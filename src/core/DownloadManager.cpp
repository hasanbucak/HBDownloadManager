#include "DownloadManager.h"
#include "MediaHosts.h"
#include "PathUtils.h"
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
#include <QSet>
#include <QRegularExpression>
#include <sys/stat.h>

namespace {
// Engine-owned fields: the downloader is the source of truth for these while it runs
void copyEngineState(DownloadItem &dst, const DownloadItem &src) {
    dst.m_segments = src.m_segments;
    dst.m_receivedBytes = src.m_receivedBytes;
    dst.m_totalBytes = src.m_totalBytes;
    dst.m_supportsRanges = src.m_supportsRanges;
    dst.m_etag = src.m_etag;
    if (src.m_url.isValid() && !src.m_url.isEmpty()) {
        dst.m_url = src.m_url;
    }
    if (!src.m_fileName.isEmpty()) {
        dst.m_fileName = src.m_fileName;
    }
    dst.m_fileNameExplicit = src.m_fileNameExplicit;
}
}

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
    cleanupOrphanParts();
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
    // The same link sent twice while the first copy is still in flight (double click on the page
    // button, browser and clipboard both firing) would write the same target twice
    for (int i = 0; i < m_items.size(); ++i) {
        const DownloadItem &other = m_items.at(i);
        const bool inFlight = (other.m_state == DownloadStateQueued || other.isActive());
        const bool sameKind = (item.m_kind == DownloadKindUnspecified || other.m_kind == item.m_kind);
        if (inFlight && sameKind
            && (other.m_originalUrl == item.m_url || other.m_url == item.m_url)) {
            return other.m_id;
        }
    }

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
        downloader->pause();  // closes the part file with fdatasync: counters are durable now
        DownloadItem *item = findItem(id);
        if (item != NULL) {
            copyEngineState(*item, downloader->durableItem());
        }
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
        DownloadItem *item = findItem(id);
        if (item != NULL) {
            item->m_tempPaths = downloader->item().m_tempPaths;
        }
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

    // Whatever was running or waiting goes back into the queue at the next start
    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_state == DownloadStateQueued || m_items.at(i).isActive()) {
            m_items[i].m_resumeOnStart = true;
        }
    }

    // Signal every yt-dlp process group first, so the waits below overlap instead of adding up
    for (QMap<QString, VideoDownloader*>::iterator it = m_activeVideo.begin(); it != m_activeVideo.end(); ++it) {
        if (it.value() != NULL) {
            it.value()->requestStop();
        }
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
    const bool isVideo = (found->m_kind == DownloadKindVideo || found->m_kind == DownloadKindAudio);
    const QString finalPath = found->m_finalPath;
    const QString directory = found->m_directory;
    const QString partPath = found->partFilePath();

    stopDownloader(id);  // also collects the yt-dlp file list of a running video
    found = findItem(id);
    const QStringList tempPaths = (found != NULL ? found->m_tempPaths : QStringList());

    for (int i = 0; i < m_items.size(); ++i) {
        if (m_items.at(i).m_id == id) {
            m_items.removeAt(i);
            break;
        }
    }

    // Once the item leaves the list nothing can resume it, so its .part is always removed.
    // The UI asks before removing a non-completed item.
    if (QFile::exists(partPath)) {
        QFile::remove(partPath);
    }

    // Files another entry still points to (the same video added twice) are never removed
    QSet<QString> inUse;
    for (int i = 0; i < m_items.size(); ++i) {
        if (!m_items.at(i).m_finalPath.isEmpty()) {
            inUse.insert(QFileInfo(m_items.at(i).m_finalPath).absoluteFilePath());
        }
        // ...nor the files of another video that is still downloading
        for (int t = 0; t < m_items.at(i).m_tempPaths.size(); ++t) {
            inUse.insert(QFileInfo(m_items.at(i).m_tempPaths.at(t)).absoluteFilePath());
        }
    }

    // An unfinished video leaves yt-dlp's own files (format files, .part, .ytdl, fragments,
    // the merger's .temp output): they can be gigabytes and nothing else would ever remove them
    if (isVideo && !wasCompleted) {
        removeVideoLeftovers(tempPaths, directory, inUse);
    }

    // The final file only when explicitly asked, only for a Completed item, and only inside its folder
    if (deleteFile && wasCompleted && !finalPath.isEmpty()
        && PathUtils::isInsideDir(finalPath, directory)
        && !inUse.contains(QFileInfo(finalPath).absoluteFilePath())) {
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
        it->m_etag = res.m_etag;
        if (!res.m_suggestedFileName.isEmpty()) {
            it->m_fileName = res.m_suggestedFileName;
        }
        it->m_fileNameExplicit = true;  // decided at the first probe; a resume keeps the name
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

void DownloadManager::syncVideoTempPaths(DownloadItem *it, QObject *downloader) {
    VideoDownloader *vd = qobject_cast<VideoDownloader*>(downloader);
    if (it != NULL && vd != NULL) {
        it->m_tempPaths = vd->item().m_tempPaths;
    }
}

void DownloadManager::removeVideoLeftovers(const QStringList &recorded, const QString &directory,
                                           const QSet<QString> &inUse) {
    // yt-dlp names format files "<stem>.f<format id>.<ext>". The next format may already be on disk
    // before its "Destination" line reached us (the process is stopped mid-way), so every sibling
    // format file of a recorded one counts as ours too.
    // Format id with a digit (f137, f0, fhls-720) and a media extension: "my.file.mp4" is no format file
    static const QString kFmt = QStringLiteral("\\.f[A-Za-z_-]*[0-9][A-Za-z0-9_-]*\\.(?:mp4|m4a|webm|mkv|mp3|opus|ogg|aac|flv|3gp|ts|mov|wav|flac)");
    static const QRegularExpression formatFile(QStringLiteral("^(.+)") + kFmt + QStringLiteral("$"));
    static const QRegularExpression formatTail(QStringLiteral("^") + kFmt + QStringLiteral("(\\.part.*|\\.ytdl)?$"));
    QStringList paths = recorded;
    for (int i = 0; i < recorded.size(); ++i) {
        const QFileInfo fi(recorded.at(i));
        const QRegularExpressionMatch m = formatFile.match(fi.fileName());
        if (!m.hasMatch()) continue;
        const QString stem = m.captured(1);
        const QStringList entries = QDir(fi.absolutePath()).entryList(QDir::Files | QDir::Hidden);
        for (int e = 0; e < entries.size(); ++e) {
            const QString &name = entries.at(e);
            if (name.startsWith(stem) && formatTail.match(name.mid(stem.size())).hasMatch()) {
                // Reduce "<x>.f1.m4a.part-Frag2.part" to its format file "<x>.f1.m4a"
                QString formatName = name;
                const int partPos = formatName.indexOf(QStringLiteral(".part"), stem.size());
                if (partPos > 0) formatName = formatName.left(partPos);
                if (formatName.endsWith(QStringLiteral(".ytdl"))) formatName.chop(5);
                const QString full = QDir(fi.absolutePath()).filePath(formatName);
                if (!paths.contains(full)) paths.append(full);
            }
        }
    }

    for (int i = 0; i < paths.size(); ++i) {
        const QFileInfo base(paths.at(i));
        if (base.fileName().isEmpty()) continue;
        QStringList victims;
        victims << base.absoluteFilePath()
                << base.absoluteFilePath() + ".part"
                << base.absoluteFilePath() + ".ytdl"
                << QDir(base.absolutePath()).filePath(base.completeBaseName() + ".temp." + base.suffix());
        // Plain prefix match: titles often contain "[...]", which a name filter reads as a wildcard
        const QString fragPrefix = base.fileName() + ".part-Frag";
        const QStringList entries = QDir(base.absolutePath()).entryList(QDir::Files | QDir::Hidden);
        for (int f = 0; f < entries.size(); ++f) {
            if (entries.at(f).startsWith(fragPrefix)) {
                victims << QDir(base.absolutePath()).filePath(entries.at(f));
            }
        }
        for (int v = 0; v < victims.size(); ++v) {
            const QString &victim = victims.at(v);
            if (inUse.contains(QFileInfo(victim).absoluteFilePath())) continue;
            if (!PathUtils::isInsideDir(victim, directory)) continue;
            if (QFile::exists(victim)) {
                QFile::remove(victim);
            }
        }
    }
}

void DownloadManager::onVideoTitleDetected(const QString &title) {
    QString id = findIdByDownloader(sender());
    DownloadItem *it = findItem(id);
    syncVideoTempPaths(it, sender());
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
    syncVideoTempPaths(it, sender());
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
        // Kept after a failure so that removing the entry still cleans up; no longer needed once done
        it->m_tempPaths = (success ? QStringList() : updated.m_tempPaths);

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
        const DownloadItem &item = m_items.at(i);
        SegmentedDownloader *dl = m_activeSegmented.value(item.m_id, NULL);
        if (dl != NULL) {
            // Persist only counters whose bytes are known to be on disk
            DownloadItem durable = item;
            copyEngineState(durable, dl->durableItem());
            array.append(durable.toJson());
        } else {
            array.append(item.toJson());
        }
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

    // D7: drop corrupt state copies older than 7 days (they may contain session cookies)
    {
        QDir cfgDir(QFileInfo(path).absolutePath());
        const QStringList oldCorrupt = cfgDir.entryList(QStringList() << "downloads.json.corrupt-*", QDir::Files);
        const QDateTime now = QDateTime::currentDateTime();
        for (int i = 0; i < oldCorrupt.size(); ++i) {
            QFileInfo cInfo(cfgDir.filePath(oldCorrupt.at(i)));
            if (cInfo.lastModified().daysTo(now) > 7) {
                cfgDir.remove(oldCorrupt.at(i));
            }
        }
    }

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

void DownloadManager::cleanupOrphanParts() {
    // Hidden ".dm-<id>.part" files whose item is no longer in the list can never be resumed
    // Only folders that loaded items point to (never a blanket sweep of the default folder: a test
    // process or a fresh profile must not touch parts it does not know), and only files older than a day.
    QSet<QString> knownIds;
    QSet<QString> dirs;
    for (int i = 0; i < m_items.size(); ++i) {
        knownIds.insert(m_items.at(i).m_id);
        if (!m_items.at(i).m_directory.isEmpty()) {
            dirs.insert(m_items.at(i).m_directory);
        }
    }

    static const QRegularExpression partRegex(QStringLiteral("^\\.dm-([A-Za-z0-9_-]{1,64})\\.part$"));
    for (QSet<QString>::const_iterator dIt = dirs.constBegin(); dIt != dirs.constEnd(); ++dIt) {
        QDir dir(*dIt);
        if (dir.path().isEmpty() || !dir.exists()) continue;
        const QStringList parts = dir.entryList(QStringList() << ".dm-*.part", QDir::Files | QDir::Hidden);
        for (int i = 0; i < parts.size(); ++i) {
            const QRegularExpressionMatch m = partRegex.match(parts.at(i));
            const QFileInfo info(dir.filePath(parts.at(i)));
            if (m.hasMatch() && !knownIds.contains(m.captured(1))
                && info.lastModified().secsTo(QDateTime::currentDateTime()) > 24 * 3600) {
                dir.remove(parts.at(i));
            }
        }
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
    processQueue();  // a higher limit starts waiting items right away
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
