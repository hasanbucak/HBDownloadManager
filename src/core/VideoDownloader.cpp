#include "VideoDownloader.h"
#include "PathUtils.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>

VideoDownloader::VideoDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent),
      m_item(item),
      m_process(new QProcess(this)),
      m_tempCookieFile(NULL),
      m_cancelled(false),
      m_paused(false),
      m_videoTotalBytes(0),
      m_audioTotalBytes(0),
      m_isAudioPhase(false) {
    connect(m_process, SIGNAL(readyReadStandardOutput()), this, SLOT(onProcessReadyRead()));
    connect(m_process, SIGNAL(readyReadStandardError()), this, SLOT(onProcessReadyRead()));
    connect(m_process, SIGNAL(finished(int, QProcess::ExitStatus)),
            this, SLOT(onProcessFinished(int, QProcess::ExitStatus)));
}

VideoDownloader::~VideoDownloader() {
    cancel();
    if (m_tempCookieFile != NULL) {
        delete m_tempCookieFile;
        m_tempCookieFile = NULL;
    }
}

QString VideoDownloader::findYtDlp() {
    QString customHome = QDir::homePath() + "/.local/bin/yt-dlp";
    if (QFile::exists(customHome)) return customHome;

    QString inPath = QStandardPaths::findExecutable("yt-dlp");
    if (!inPath.isEmpty()) return inPath;

    if (QFile::exists("/usr/local/bin/yt-dlp")) return "/usr/local/bin/yt-dlp";
    if (QFile::exists("/usr/bin/yt-dlp")) return "/usr/bin/yt-dlp";

    return QString();
}

QString VideoDownloader::findFfmpeg() {
    QString customHome = QDir::homePath() + "/.local/bin/ffmpeg";
    if (QFile::exists(customHome)) return customHome;

    QString inPath = QStandardPaths::findExecutable("ffmpeg");
    if (!inPath.isEmpty()) return inPath;

    if (QFile::exists("/usr/local/bin/ffmpeg")) return "/usr/local/bin/ffmpeg";
    if (QFile::exists("/usr/bin/ffmpeg")) return "/usr/bin/ffmpeg";

    return QString();
}

QString VideoDownloader::findNode() {
    QString inPath = QStandardPaths::findExecutable("node");
    if (!inPath.isEmpty()) return inPath;

    if (QFile::exists("/usr/bin/node")) return "/usr/bin/node";
    if (QFile::exists("/usr/local/bin/node")) return "/usr/local/bin/node";

    return QString();
}

bool VideoDownloader::isAvailable() {
    return !findYtDlp().isEmpty();
}

QString VideoDownloader::createCookieFile() {
    if (m_item.m_cookies.isEmpty()) return QString();

    m_tempCookieFile = new QTemporaryFile(this);
    if (!m_tempCookieFile->open()) return QString();

    QTextStream out(m_tempCookieFile);
    out << "# Netscape HTTP Cookie File\n";
    for (int i = 0; i < m_item.m_cookies.size(); ++i) {
        QJsonObject c = m_item.m_cookies.at(i).toObject();
        QString domain = c["domain"].toString();
        bool includeSub = domain.startsWith('.');
        QString path = c["path"].toString("/");
        bool secure = c["secure"].toBool();
        qint64 expiry = c["expirationDate"].toVariant().toLongLong();
        if (expiry <= 0) expiry = 2147483647;
        QString name = c["name"].toString();
        QString value = c["value"].toString();

        out << domain << "\t"
            << (includeSub ? "TRUE" : "FALSE") << "\t"
            << path << "\t"
            << (secure ? "TRUE" : "FALSE") << "\t"
            << expiry << "\t"
            << name << "\t"
            << value << "\n";
    }
    m_tempCookieFile->flush();
    return m_tempCookieFile->fileName();
}

void VideoDownloader::start() {
    QString ytdlp = findYtDlp();
    if (ytdlp.isEmpty()) {
        emit atFinished(false, QString(), tr("yt-dlp bulunamadı. Lütfen sisteminize yt-dlp kurun."));
        return;
    }

    m_cancelled = false;
    m_paused = false;
    m_videoTotalBytes = 0;
    m_audioTotalBytes = 0;
    m_isAudioPhase = false;
    m_recentOutputLines.clear();
    m_finalPath.clear();

    QString qual = m_item.m_quality.trimmed().toLower();
    int heightNum = 0;
    QRegularExpression hRegex("(\\d{3,4})");
    QRegularExpressionMatch hMatch = hRegex.match(qual);
    if (hMatch.hasMatch()) {
        heightNum = hMatch.captured(1).toInt();
    }

    QString qualityTag;
    if (m_item.m_kind == DownloadKindAudio) {
        qualityTag = " [Ses]";
    } else if (heightNum > 0) {
        qualityTag = QString(" [%1p]").arg(heightNum);
    } else {
        qualityTag = " [%(height)sp]";
    }

    QString outputTemplate = QDir(m_item.m_directory).filePath("%(title)s" + qualityTag + ".%(ext)s");

    QStringList args;
    args << "--ignore-config"
         << "--newline"
         << "--progress"
         << "--no-playlist"
         << "--continue"
         << "--part"
         << "--windows-filenames"
         << "--no-mtime"
         << "--print" << "TITLE:%(title)s"
         << "--print" << "after_move:HBDM_FILE:%(filepath)s"
         << "--progress-template"
         << "download:HBDM_PROG:%(progress.downloaded_bytes)s|%(progress.total_bytes)s|%(progress.total_bytes_estimate)s|%(progress._speed_str)s|%(progress._eta_str)s|%(progress._percent_str)s|%(progress.filename)s"
         << "--no-simulate";

    if (m_item.m_kind == DownloadKindAudio) {
        args << "-x" << "--audio-format" << "mp3";
    } else if (heightNum > 0) {
        args << "--format" << QString("bestvideo[height<=%1][ext=mp4]+bestaudio[ext=m4a]/bestvideo[height<=%1]+bestaudio/best[height<=%1][ext=mp4]/best[height<=%1]/best").arg(heightNum);
        args << "--merge-output-format" << "mp4";
    } else {
        args << "--format" << "bestvideo[ext=mp4]+bestaudio[ext=m4a]/best[ext=mp4]/bestvideo+bestaudio/best";
        args << "--merge-output-format" << "mp4";
    }

    args << "-o" << outputTemplate;

    QString ffmpegPath = findFfmpeg();
    if (!ffmpegPath.isEmpty()) {
        args << "--ffmpeg-location" << QFileInfo(ffmpegPath).absolutePath();
    }

    QString nodePath = findNode();
    if (!nodePath.isEmpty()) {
        args << "--js-runtimes" << QString("node:%1").arg(nodePath);
    }

    QString cookiePath = createCookieFile();
    if (!cookiePath.isEmpty()) {
        args << "--cookies" << cookiePath;
    } else if (!m_item.m_cookieHeader.isEmpty()) {
        args << "--add-header" << QString("Cookie:%1").arg(m_item.m_cookieHeader);
    }

    if (!m_item.m_headers.value("User-Agent").isEmpty()) {
        args << "--user-agent" << m_item.m_headers.value("User-Agent");
    }

    if (!m_item.m_headers.value("Referer").isEmpty()) {
        args << "--referer" << m_item.m_headers.value("Referer");
    }

    args << "--" << m_item.m_url.toString();

    m_process->setProcessChannelMode(QProcess::MergedChannels);
    m_process->start(ytdlp, args);
}

void VideoDownloader::pause() {
    if (m_process != NULL && m_process->state() != QProcess::NotRunning) {
        m_paused = true;
        m_process->terminate();
        if (!m_process->waitForFinished(1500)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
}

void VideoDownloader::cancel() {
    if (m_process != NULL && m_process->state() != QProcess::NotRunning) {
        m_cancelled = true;
        m_process->terminate();
        if (!m_process->waitForFinished(1500)) {
            m_process->kill();
            m_process->waitForFinished(500);
        }
    }
}

void VideoDownloader::onProcessReadyRead() {
    while (m_process->canReadLine()) {
        QString line = QString::fromUtf8(m_process->readLine()).trimmed();
        if (!line.isEmpty()) {
            parseOutputLine(line);
        }
    }
}

static qint64 parseSizeStringToBytes(const QString &sizeStr) {
    QString s = sizeStr.trimmed();
    while (s.startsWith('~') || s.startsWith(' ')) s = s.mid(1).trimmed();
    double val = 0.0;
    if (s.endsWith("GiB", Qt::CaseInsensitive) || s.endsWith("GB", Qt::CaseInsensitive)) {
        int cut = s.endsWith("GiB", Qt::CaseInsensitive) ? 3 : 2;
        val = s.left(s.length() - cut).trimmed().toDouble() * 1024.0 * 1024.0 * 1024.0;
    } else if (s.endsWith("MiB", Qt::CaseInsensitive) || s.endsWith("MB", Qt::CaseInsensitive)) {
        int cut = s.endsWith("MiB", Qt::CaseInsensitive) ? 3 : 2;
        val = s.left(s.length() - cut).trimmed().toDouble() * 1024.0 * 1024.0;
    } else if (s.endsWith("KiB", Qt::CaseInsensitive) || s.endsWith("KB", Qt::CaseInsensitive)) {
        int cut = s.endsWith("KiB", Qt::CaseInsensitive) ? 3 : 2;
        val = s.left(s.length() - cut).trimmed().toDouble() * 1024.0;
    } else if (s.endsWith("B", Qt::CaseInsensitive)) {
        val = s.left(s.length() - 1).trimmed().toDouble();
    }
    return static_cast<qint64>(val);
}

void VideoDownloader::parseOutputLine(const QString &line) {
    bool isProgress = line.startsWith("HBDM_PROG:") ||
                      (line.startsWith("[download]") && (line.contains('%') || line.contains("ETA")));
    if (!isProgress) {
        m_recentOutputLines.append(line);
        while (m_recentOutputLines.size() > 20) {
            m_recentOutputLines.removeFirst();
        }
    }

    if (line.startsWith("TITLE:")) {
        QString rawTitle = line.mid(6).trimmed();
        if (!rawTitle.isEmpty()) {
            QString safeTitle = PathUtils::sanitizeFileName(rawTitle);
            QString qualTag;
            QString qual = m_item.m_quality.trimmed().toLower();
            int heightNum = 0;
            QRegularExpression hRegex("(\\d{3,4})");
            QRegularExpressionMatch hMatch = hRegex.match(qual);
            if (hMatch.hasMatch()) {
                heightNum = hMatch.captured(1).toInt();
            }

            if (m_item.m_kind == DownloadKindAudio) {
                qualTag = " [Ses]";
            } else if (heightNum > 0) {
                qualTag = QString(" [%1p]").arg(heightNum);
            } else if (qual == "best" || qual.isEmpty()) {
                qualTag = " [En Yüksek]";
            }
            QString ext = (m_item.m_kind == DownloadKindAudio ? ".mp3" : ".mp4");
            m_item.m_fileName = safeTitle + qualTag + ext;
            emit atTitleDetected(m_item.m_fileName);
        }
        return;
    }

    if (line.startsWith("HBDM_FILE:")) {
        QString candidate = line.mid(10).trimmed();
        QFileInfo fi(candidate);
        QString canonicalDir = QFileInfo(m_item.m_directory).canonicalFilePath();
        QString canonicalFile = fi.canonicalFilePath();
        if (canonicalFile.isEmpty()) {
            canonicalFile = fi.absoluteFilePath();
        }
        if (!canonicalDir.isEmpty() && canonicalFile.startsWith(canonicalDir + '/')) {
            m_finalPath = canonicalFile;
            m_item.m_fileName = fi.fileName();
            emit atTitleDetected(m_item.m_fileName);
        }
        return;
    }

    if (line.startsWith("HBDM_PROG:")) {
        QString data = line.mid(10).trimmed();
        QStringList parts = data.split('|');
        if (parts.size() >= 6) {
            qint64 downloaded = (parts[0] != "NA" && parts[0] != "Unknown") ? parts[0].trimmed().toLongLong() : 0;
            qint64 total = (parts[1] != "NA" && parts[1] != "Unknown") ? parts[1].trimmed().toLongLong() : 0;
            if (total <= 0 && parts.size() > 2 && parts[2] != "NA" && parts[2] != "Unknown") {
                total = parts[2].trimmed().toLongLong();
            }
            QString speed = (parts.size() > 3 && parts[3] != "NA" && parts[3] != "Unknown") ? parts[3].trimmed() : "";
            QString eta = (parts.size() > 4 && parts[4] != "NA" && parts[4] != "Unknown") ? parts[4].trimmed() : "";
            QString pctStr = (parts.size() > 5) ? parts[5].trimmed() : "0";
            if (pctStr.endsWith('%')) pctStr = pctStr.left(pctStr.length() - 1).trimmed();
            double pct = pctStr.toDouble();

            if (parts.size() >= 7 && !parts[6].isEmpty() && parts[6] != "NA") {
                m_finalPath = parts[6].trimmed();
                if (m_finalPath.endsWith(".m4a") || m_finalPath.endsWith(".opus") || m_finalPath.endsWith(".mp3") ||
                    m_finalPath.contains(".f140") || m_finalPath.contains(".f249") || m_finalPath.contains(".f250") || m_finalPath.contains(".f251") ||
                    (m_videoTotalBytes > 0 && m_item.m_kind != DownloadKindAudio)) {
                    m_isAudioPhase = true;
                }
                m_item.m_fileName = QFileInfo(m_finalPath).fileName();
                emit atTitleDetected(m_item.m_fileName);
            }

            if (m_isAudioPhase && m_item.m_kind != DownloadKindAudio) {
                m_audioTotalBytes = total;
                qint64 overallTotal = (m_videoTotalBytes > 0 ? (m_videoTotalBytes + m_audioTotalBytes)
                                       : (m_item.m_totalBytes > 0 ? m_item.m_totalBytes : m_audioTotalBytes));
                double videoWeight = (m_videoTotalBytes > 0 && overallTotal > 0)
                                     ? (static_cast<double>(m_videoTotalBytes) / static_cast<double>(overallTotal))
                                     : 0.95;
                double overallPct = (videoWeight * 100.0) + ((1.0 - videoWeight) * pct);
                m_item.m_videoPercent = overallPct;
                m_item.m_totalBytes = overallTotal;
                m_item.m_receivedBytes = (overallTotal > 0 ? static_cast<qint64>((overallPct / 100.0) * overallTotal) : (m_videoTotalBytes + downloaded));
                m_item.m_stage = tr("Ses indiriliyor...");
                emit atProgressUpdated(overallPct, overallTotal, m_item.m_receivedBytes, speed, eta, m_item.m_stage);
                return;
            } else {
                m_videoTotalBytes = (total > 0 ? total : downloaded);
                qint64 overallTotal = total;
                if (m_item.m_totalBytes > total) {
                    overallTotal = m_item.m_totalBytes;
                }
                double overallPct = pct;
                if (m_item.m_kind != DownloadKindAudio && m_item.m_totalBytes > total && m_item.m_totalBytes > 0) {
                    overallPct = pct * (static_cast<double>(total) / static_cast<double>(m_item.m_totalBytes));
                }
                m_item.m_videoPercent = overallPct;
                m_item.m_totalBytes = overallTotal;
                m_item.m_receivedBytes = (overallTotal > 0 ? static_cast<qint64>((overallPct / 100.0) * overallTotal) : downloaded);
                m_item.m_stage = (m_item.m_kind == DownloadKindAudio ? tr("Ses indiriliyor...") : tr("Video indiriliyor..."));
                emit atProgressUpdated(overallPct, overallTotal, m_item.m_receivedBytes, speed, eta, m_item.m_stage);
                return;
            }
        }
    }

    if (line.contains("Resuming download")) {
        m_item.m_stage = tr("Kaldığı yerden devam ediliyor...");
        emit atProgressUpdated(m_item.m_videoPercent, m_item.m_totalBytes, m_item.m_receivedBytes, "", "", m_item.m_stage);
        return;
    }

    static QRegularExpression destRegex(R"(\[download\] Destination:\s+(.+))");
    QRegularExpressionMatch match = destRegex.match(line);
    if (match.hasMatch()) {
        m_finalPath = match.captured(1).trimmed();
        if (m_finalPath.endsWith(".m4a") || m_finalPath.endsWith(".opus") || m_finalPath.endsWith(".mp3") ||
            m_finalPath.contains(".f140") || m_finalPath.contains(".f249") || m_finalPath.contains(".f250") || m_finalPath.contains(".f251") ||
            (m_videoTotalBytes > 0 && m_item.m_kind != DownloadKindAudio)) {
            m_isAudioPhase = true;
        }
        m_item.m_fileName = QFileInfo(m_finalPath).fileName();
        emit atTitleDetected(m_item.m_fileName);
        return;
    }

    static QRegularExpression progressRegex(R"(\[download\]\s+([\d\.]+)%\s+of\s+(?:~\s*)?([^\s]+)\s+at\s+([^\s]+)(?:\s+ETA\s+([^\s]+))?)");
    match = progressRegex.match(line);
    if (match.hasMatch()) {
        double pct = match.captured(1).toDouble();
        qint64 total = parseSizeStringToBytes(match.captured(2));
        QString speed = match.captured(3);
        QString eta = match.captured(4);

        if (m_isAudioPhase && m_item.m_kind != DownloadKindAudio) {
            m_audioTotalBytes = total;
            qint64 overallTotal = (m_videoTotalBytes > 0 ? (m_videoTotalBytes + m_audioTotalBytes)
                                   : (m_item.m_totalBytes > 0 ? m_item.m_totalBytes : m_audioTotalBytes));
            double videoWeight = (m_videoTotalBytes > 0 && overallTotal > 0)
                                 ? (static_cast<double>(m_videoTotalBytes) / static_cast<double>(overallTotal))
                                 : 0.95;
            double overallPct = (videoWeight * 100.0) + ((1.0 - videoWeight) * pct);
            m_item.m_videoPercent = overallPct;
            m_item.m_totalBytes = overallTotal;
            m_item.m_receivedBytes = static_cast<qint64>((overallPct / 100.0) * overallTotal);
            m_item.m_stage = tr("Ses indiriliyor...");
            emit atProgressUpdated(overallPct, overallTotal, m_item.m_receivedBytes, speed, eta, m_item.m_stage);
            return;
        } else {
            m_videoTotalBytes = total;
            qint64 overallTotal = total;
            if (m_item.m_totalBytes > total) {
                overallTotal = m_item.m_totalBytes;
            }
            double overallPct = pct;
            if (m_item.m_kind != DownloadKindAudio && m_item.m_totalBytes > total && m_item.m_totalBytes > 0) {
                overallPct = pct * (static_cast<double>(total) / static_cast<double>(m_item.m_totalBytes));
            }
            m_item.m_videoPercent = overallPct;
            m_item.m_totalBytes = overallTotal;
            m_item.m_receivedBytes = static_cast<qint64>((overallPct / 100.0) * overallTotal);
            m_item.m_stage = (m_item.m_kind == DownloadKindAudio ? tr("Ses indiriliyor...") : tr("Video indiriliyor..."));
            emit atProgressUpdated(overallPct, overallTotal, m_item.m_receivedBytes, speed, eta, m_item.m_stage);
            return;
        }
    }

    static QRegularExpression alreadyRegex(R"(\[download\]\s+(.+?)\s+has already been downloaded)");
    QRegularExpressionMatch alreadyMatch = alreadyRegex.match(line);
    if (alreadyMatch.hasMatch()) {
        m_finalPath = alreadyMatch.captured(1).trimmed();
        m_item.m_fileName = QFileInfo(m_finalPath).fileName();
        emit atTitleDetected(m_item.m_fileName);
        return;
    }

    static QRegularExpression mergeRegex("\\[Merger\\] Merging formats into \"([^\"]+)\"");
    match = mergeRegex.match(line);
    if (match.hasMatch()) {
        m_finalPath = match.captured(1).trimmed();
        m_item.m_fileName = QFileInfo(m_finalPath).fileName();
        emit atTitleDetected(m_item.m_fileName);
        m_item.m_stage = tr("Ses ve görüntü birleştiriliyor...");
        emit atProgressUpdated(99.0, m_item.m_totalBytes, m_item.m_totalBytes, "", "", m_item.m_stage);
        return;
    }

    if (line.contains("[download] 100%")) {
        emit atProgressUpdated(100.0, m_item.m_totalBytes, m_item.m_totalBytes, "", "", tr("Tamamlanıyor..."));
    }
}

void VideoDownloader::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    while (m_process->canReadLine()) {
        QString line = QString::fromUtf8(m_process->readLine()).trimmed();
        if (!line.isEmpty()) {
            parseOutputLine(line);
        }
    }
    QByteArray remaining = m_process->readAll();
    if (!remaining.isEmpty()) {
        QStringList remLines = QString::fromUtf8(remaining).split('\n');
        for (int i = 0; i < remLines.size(); ++i) {
            QString rLine = remLines.at(i).trimmed();
            if (!rLine.isEmpty()) {
                parseOutputLine(rLine);
            }
        }
    }

    if (m_paused) {
        return;
    }

    if (m_cancelled) {
        emit atFinished(false, QString(), tr("İptal edildi"));
        return;
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        if (m_finalPath.isEmpty()) {
            m_finalPath = QDir(m_item.m_directory).filePath(m_item.m_fileName);
        }
        if (QFile::exists(m_finalPath)) {
            QFileInfo fi(m_finalPath);
            m_item.m_totalBytes = fi.size();
            m_item.m_receivedBytes = fi.size();
            m_item.m_fileName = fi.fileName();
            emit atFinished(true, m_finalPath, QString());
        } else {
            emit atFinished(false, QString(), tr("İndirilen dosya bulunamadı: %1").arg(m_finalPath));
        }
    } else {
        QString err;
        for (int i = m_recentOutputLines.size() - 1; i >= 0; --i) {
            const QString &l = m_recentOutputLines.at(i);
            if (l.startsWith("ERROR:", Qt::CaseInsensitive)) {
                err = l;
                break;
            }
        }
        if (err.isEmpty() && !m_recentOutputLines.isEmpty()) {
            for (int i = m_recentOutputLines.size() - 1; i >= 0; --i) {
                const QString &l = m_recentOutputLines.at(i);
                if (l.contains("ERROR", Qt::CaseInsensitive)) {
                    err = l;
                    break;
                }
            }
            if (err.isEmpty()) {
                err = m_recentOutputLines.last();
            }
        }
        if (err.isEmpty()) {
            err = tr("yt-dlp işlemi hata kodu %1 ile sonlandı").arg(exitCode);
        }
        emit atFinished(false, QString(), err);
    }
}
