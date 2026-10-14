#include "DownloadItem.h"
#include "PathUtils.h"
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QStringList>
#include <QRegularExpression>
#include <QCoreApplication>

DownloadItem::DownloadItem()
    : m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_kind(DownloadKindUnspecified),
      m_state(DownloadStateQueued),
      m_totalBytes(-1),
      m_receivedBytes(0),
      m_supportsRanges(false),
      m_createdAt(QDateTime::currentDateTime()),
      m_requestedSegments(8),
      m_originalUrl(),
      m_etag(),
      m_fileNameExplicit(false),
      m_resumeOnStart(false),
      m_speed(0.0),
      m_videoPercent(0.0),
      m_lastReceived(0),
      m_lastSpeedTime(0) {
    if (m_directory.isEmpty()) {
        m_directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    }
}

DownloadItem::DownloadItem(const QUrl &url, const QString &dir)
    : m_id(QUuid::createUuid().toString(QUuid::WithoutBraces)),
      m_url(url),
      m_kind(DownloadKindUnspecified),
      m_state(DownloadStateQueued),
      m_totalBytes(-1),
      m_receivedBytes(0),
      m_supportsRanges(false),
      m_createdAt(QDateTime::currentDateTime()),
      m_requestedSegments(8),
      m_originalUrl(url),
      m_etag(),
      m_fileNameExplicit(false),
      m_resumeOnStart(false),
      m_speed(0.0),
      m_videoPercent(0.0),
      m_lastReceived(0),
      m_lastSpeedTime(0) {
    if (dir.isEmpty()) {
        m_directory = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    } else {
        m_directory = dir;
    }

    m_fileName = PathUtils::sanitizeFileName(QFileInfo(url.path()).fileName());
}

bool DownloadItem::isActive() const {
    return (m_state == DownloadStateConnecting || m_state == DownloadStateDownloading);
}

double DownloadItem::progress() const {
    if (m_kind == DownloadKindVideo || m_kind == DownloadKindAudio) {
        if (m_state == DownloadStateCompleted) return 1.0;
        return qBound(0.0, m_videoPercent / 100.0, 1.0);
    }
    if (m_totalBytes <= 0) return 0.0;
    return qBound(0.0, static_cast<double>(m_receivedBytes) / static_cast<double>(m_totalBytes), 1.0);
}

qint64 DownloadItem::etaSeconds() const {
    if (m_totalBytes <= 0 || m_speed <= 1024.0) return -1;
    qint64 remaining = m_totalBytes - m_receivedBytes;
    if (remaining <= 0) return 0;
    return static_cast<qint64>(remaining / m_speed);
}

QString DownloadItem::stateString() const {
    switch (m_state) {
    case DownloadStateQueued: return QObject::tr("Kuyrukta");
    case DownloadStateConnecting: return QObject::tr("Bağlanıyor...");
    case DownloadStateDownloading: return QObject::tr("İndiriliyor");
    case DownloadStatePaused: return QObject::tr("Duraklatıldı");
    case DownloadStateCompleted: return QObject::tr("Tamamlandı");
    case DownloadStateError: return QObject::tr("Hata");
    case DownloadStateCancelled: return QObject::tr("İptal Edildi");
    }
    return QString();
}

QString DownloadItem::formattedProgress() const {
    if (m_totalBytes <= 0) {
        return QObject::tr("%1 indirildi").arg(formattedSize());
    }
    return QString::number(progress() * 100.0, 'f', 1) + "%";
}

static QString formatBytes(qint64 bytes) {
    if (bytes < 0) return "--";
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(QString::number(bytes / 1024.0, 'f', 1));
    if (bytes < 1024 * 1024 * 1024) return QString("%1 MB").arg(QString::number(bytes / (1024.0 * 1024.0), 'f', 2));
    return QString("%1 GB").arg(QString::number(bytes / (1024.0 * 1024.0 * 1024.0), 'f', 2));
}

QString DownloadItem::formattedSize() const {
    if (m_totalBytes > 0) {
        // Finished: the size alone. Running: "2.17 / 28.61 MB" (one unit when both share it),
        // short enough that the file name keeps the room in the list.
        if (m_state == DownloadStateCompleted || m_receivedBytes >= m_totalBytes) {
            return formatBytes(m_totalBytes);
        }
        const QString done = formatBytes(m_receivedBytes);
        const QString total = formatBytes(m_totalBytes);
        const QString doneUnit = done.section(' ', 1);
        if (!doneUnit.isEmpty() && doneUnit == total.section(' ', 1)) {
            return QString("%1 / %2").arg(done.section(' ', 0, 0), total);
        }
        return QString("%1 / %2").arg(done, total);
    }
    return formatBytes(m_receivedBytes);
}

QString DownloadItem::formattedSpeed() const {
    if (!isActive() || m_speed <= 0) return "--";
    return formatBytes(static_cast<qint64>(m_speed)) + "/s";
}

QString DownloadItem::formattedEta() const {
    if (!isActive()) return "--";
    qint64 secs = etaSeconds();
    if (secs < 0) return "--";
    if (secs < 60) return QCoreApplication::translate("DownloadItem", "%1 sn").arg(secs);
    if (secs < 3600) return QCoreApplication::translate("DownloadItem", "%1 dk %2 sn").arg(secs / 60).arg(secs % 60);
    return QCoreApplication::translate("DownloadItem", "%1 sa %2 dk").arg(secs / 3600).arg((secs % 3600) / 60);
}

QString DownloadItem::partFilePath() const {
    QString safeId = m_id;
    safeId.remove(QRegularExpression("[^a-zA-Z0-9_-]"));
    if (safeId.isEmpty()) safeId = "unknown";
    return QDir(m_directory).filePath(QString(".dm-%1.part").arg(safeId));
}

QString DownloadItem::targetFilePath() const {
    if (!m_finalPath.isEmpty()) {
        QString safe = PathUtils::safeJoin(m_directory, QFileInfo(m_finalPath).fileName());
        if (!safe.isEmpty() && m_finalPath == safe) {
            return m_finalPath;
        }
    }
    return PathUtils::safeJoin(m_directory, m_fileName);
}

FileCategory DownloadItem::category() const {
    if (m_kind == DownloadKindAudio) return FileCategoryMusic;
    if (m_kind == DownloadKindVideo) return FileCategoryVideo;
    return detectCategory(m_fileName);
}

FileCategory DownloadItem::detectCategory(const QString &fileName) {
    QString ext = QFileInfo(fileName).suffix().toLower();
    if (ext.isEmpty()) return FileCategoryOther;

    QStringList imgExts;
    imgExts << "jpg" << "jpeg" << "png" << "gif" << "bmp" << "webp" << "svg" << "ico" << "tiff" << "heic";
    if (imgExts.contains(ext)) return FileCategoryImage;

    QStringList musicExts;
    musicExts << "mp3" << "wav" << "flac" << "aac" << "m4a" << "ogg" << "opus" << "wma";
    if (musicExts.contains(ext)) return FileCategoryMusic;

    QStringList videoExts;
    videoExts << "mp4" << "mkv" << "avi" << "mov" << "wmv" << "flv" << "webm" << "m4v" << "ts" << "mpg" << "mpeg" << "3gp";
    if (videoExts.contains(ext)) return FileCategoryVideo;

    QStringList appExts;
    appExts << "deb" << "rpm" << "appimage" << "run" << "sh" << "exe" << "msi" << "apk" << "bin";
    if (appExts.contains(ext)) return FileCategoryPrograms;

    QStringList docExts;
    docExts << "pdf" << "doc" << "docx" << "xls" << "xlsx" << "ppt" << "pptx" << "txt" << "rtf" << "odt" << "csv" << "epub";
    if (docExts.contains(ext)) return FileCategoryDocument;

    QStringList compExts;
    compExts << "zip" << "rar" << "7z" << "tar" << "gz" << "bz2" << "xz" << "tgz" << "iso" << "dmg";
    if (compExts.contains(ext)) return FileCategoryCompressed;

    return FileCategoryOther;
}

QString DownloadItem::categoryName(FileCategory cat) {
    switch (cat) {
    case FileCategoryAll: return QObject::tr("Tümü");
    case FileCategoryImage: return QObject::tr("Resimler");
    case FileCategoryMusic: return QObject::tr("Müzik");
    case FileCategoryVideo: return QObject::tr("Videolar");
    case FileCategoryPrograms: return QObject::tr("Programlar");
    case FileCategoryDocument: return QObject::tr("Belgeler");
    case FileCategoryCompressed: return QObject::tr("Sıkıştırılmış");
    case FileCategoryOther: return QObject::tr("Diğer");
    }
    return QString();
}

QJsonObject DownloadItem::toJson() const {
    QJsonObject obj;
    obj["id"] = m_id;
    obj["url"] = m_url.toString();
    obj["fileName"] = m_fileName;
    obj["directory"] = m_directory;
    if (m_kind == DownloadKindAudio) obj["kind"] = "audio";
    else if (m_kind == DownloadKindVideo) obj["kind"] = "video";
    else obj["kind"] = "file";
    obj["videoPercent"] = m_videoPercent;

    QString st;
    switch (m_state) {
    case DownloadStateQueued: st = "queued"; break;
    case DownloadStateConnecting: st = "probing"; break;
    case DownloadStateDownloading: st = "downloading"; break;
    case DownloadStatePaused: st = "paused"; break;
    case DownloadStateCompleted: st = "completed"; break;
    case DownloadStateError: st = "failed"; break;
    case DownloadStateCancelled: st = "cancelled"; break;
    }
    obj["state"] = st;
    obj["totalBytes"] = m_totalBytes;
    obj["receivedBytes"] = m_receivedBytes;
    obj["supportsRanges"] = m_supportsRanges;
    obj["errorMessage"] = m_errorMessage;
    obj["createdAt"] = m_createdAt.toString(Qt::ISODate);
    if (m_completedAt.isValid()) {
        obj["completedAt"] = m_completedAt.toString(Qt::ISODate);
    }
    obj["finalPath"] = m_finalPath;
    obj["requestedSegments"] = m_requestedSegments;
    obj["quality"] = m_quality;
    obj["originalUrl"] = m_originalUrl.toString();
    obj["etag"] = m_etag;
    obj["fileNameExplicit"] = m_fileNameExplicit;
    if (!m_tempPaths.isEmpty()) {
        obj["tempPaths"] = QJsonArray::fromStringList(m_tempPaths);
    }
    if (m_resumeOnStart) {
        obj["resumeOnStart"] = true;
    }
    obj["cookieHeader"] = m_cookieHeader;
    obj["cookies"] = m_cookies;

    QJsonObject hObj;
    for (QMap<QString, QString>::const_iterator it = m_headers.constBegin(); it != m_headers.constEnd(); ++it) {
        hObj[it.key()] = it.value();
    }
    obj["headers"] = hObj;

    QJsonArray segArray;
    for (int i = 0; i < m_segments.size(); ++i) {
        segArray.append(m_segments.at(i).toJson());
    }
    obj["segments"] = segArray;

    return obj;
}

DownloadItem DownloadItem::fromJson(const QJsonObject &obj) {
    DownloadItem item;
    static const QRegularExpression idRegex("^[a-zA-Z0-9_-]+$");
    QString rawId = obj["id"].toString();
    if (!rawId.isEmpty() && idRegex.match(rawId).hasMatch() && rawId.length() <= 64) {
        item.m_id = rawId;
    } else {
        item.m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    item.m_url = QUrl(obj["url"].toString());
    item.m_originalUrl = QUrl(obj["originalUrl"].toString());
    if (!item.m_originalUrl.isValid() || item.m_originalUrl.isEmpty()) {
        item.m_originalUrl = item.m_url;
    }
    item.m_etag = obj["etag"].toString();
    item.m_fileName = PathUtils::sanitizeFileName(obj["fileName"].toString());
    item.m_directory = obj["directory"].toString();
    QString kindStr = obj["kind"].toString();
    if (kindStr == "audio") item.m_kind = DownloadKindAudio;
    else if (kindStr == "video") item.m_kind = DownloadKindVideo;
    else item.m_kind = DownloadKindFile;
    item.m_videoPercent = obj["videoPercent"].toDouble(0.0);
    item.m_quality = obj["quality"].toString("best");

    // Items without the key predate the flag: keep their name as it is
    item.m_fileNameExplicit = obj["fileNameExplicit"].toBool(true);
    const QJsonArray tempArray = obj["tempPaths"].toArray();
    for (int i = 0; i < tempArray.size(); ++i) {
        const QString p = tempArray.at(i).toString();
        if (!p.isEmpty()) item.m_tempPaths.append(p);
    }

    // A clean quit marks what was running or queued: it goes back into the queue. A state file left
    // by a crash still says "downloading": that one waits paused (no restart loop on a bad item).
    QString st = obj["state"].toString();
    const bool resume = obj["resumeOnStart"].toBool(false);
    if (st == "downloading" || st == "probing") item.m_state = DownloadStatePaused;
    else if (st == "paused") item.m_state = resume ? DownloadStateQueued : DownloadStatePaused;
    else if (st == "completed") item.m_state = DownloadStateCompleted;
    else if (st == "failed") item.m_state = DownloadStateError;
    else if (st == "cancelled") item.m_state = DownloadStateCancelled;
    else item.m_state = DownloadStateQueued;

    item.m_totalBytes = obj["totalBytes"].toVariant().toLongLong();
    item.m_receivedBytes = obj["receivedBytes"].toVariant().toLongLong();
    item.m_supportsRanges = obj["supportsRanges"].toBool();
    item.m_errorMessage = obj["errorMessage"].toString();
    item.m_createdAt = QDateTime::fromString(obj["createdAt"].toString(), Qt::ISODate);
    if (!item.m_createdAt.isValid()) {
        item.m_createdAt = QDateTime::currentDateTime();
    }
    if (obj.contains("completedAt")) {
        item.m_completedAt = QDateTime::fromString(obj["completedAt"].toString(), Qt::ISODate);
    }
    item.m_finalPath = obj["finalPath"].toString();
    // D7: Do not wipe m_finalPath if directory doesn't exist yet (e.g. unmounted USB)
    item.m_requestedSegments = qBound(1, obj["requestedSegments"].toInt(8), 16);
    item.m_cookieHeader = obj["cookieHeader"].toString();
    item.m_cookies = obj["cookies"].toArray();

    QJsonObject hObj = obj["headers"].toObject();
    for (QJsonObject::const_iterator it = hObj.constBegin(); it != hObj.constEnd(); ++it) {
        item.m_headers[it.key()] = it.value().toString();
    }

    QJsonArray segArray = obj["segments"].toArray();
    for (int i = 0; i < segArray.size(); ++i) {
        item.m_segments.append(SegmentInfo::fromJson(segArray.at(i).toObject()));
    }

    return item;
}
