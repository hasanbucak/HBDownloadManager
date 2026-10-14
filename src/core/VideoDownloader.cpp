#include "VideoDownloader.h"
#include "PathUtils.h"
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QRandomGenerator>
#include <QCoreApplication>
#include <csignal>
#include <cerrno>
#include <sys/types.h>
#include <unistd.h>

namespace {
// yt-dlp runs in its own process group so that SIGTERM also reaches its ffmpeg children
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
class GroupProcess : public QProcess {
public:
    explicit GroupProcess(QObject *parent) : QProcess(parent) {}
protected:
    void setupChildProcess() override { ::setpgid(0, 0); }
};
#endif

QProcess *createGroupProcess(QObject *parent) {
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    return new GroupProcess(parent);
#else
    QProcess *p = new QProcess(parent);
    p->setChildProcessModifier([]() { ::setpgid(0, 0); });
    return p;
#endif
}

QString s_ytDlpVersion;
int s_ytDlpVersionState = 0;  // 0 unknown, 1 probing, 2 known
}

VideoDownloader::VideoDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent),
      m_item(item),
      m_process(createGroupProcess(this)),
      m_tempCookieFile(NULL),
      m_cancelled(false),
      m_paused(false),
      m_videoTotalBytes(0),
      m_audioTotalBytes(0),
      m_isAudioPhase(false),
      m_stopRequested(false) {
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

QString VideoDownloader::ytDlpVersion(bool allowBlocking) {
    if (s_ytDlpVersionState == 2 || !allowBlocking) {
        return s_ytDlpVersion;
    }

    QString exe = findYtDlp();
    if (exe.isEmpty()) {
        s_ytDlpVersionState = 2;
        return QString();
    }

    QProcess proc;
    proc.start(exe, QStringList() << "--version");
    if (proc.waitForFinished(2000)) {
        s_ytDlpVersion = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        s_ytDlpVersionState = 2;
    }
    return s_ytDlpVersion;
}

void VideoDownloader::prefetchYtDlpVersion() {
    if (s_ytDlpVersionState != 0 || QCoreApplication::instance() == NULL) return;

    const QString exe = findYtDlp();
    if (exe.isEmpty()) {
        s_ytDlpVersionState = 2;
        return;
    }

    s_ytDlpVersionState = 1;
    QProcess *proc = new QProcess(QCoreApplication::instance());
    QObject::connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), proc,
                     [proc](int exitCode, QProcess::ExitStatus status) {
        if (status == QProcess::NormalExit && exitCode == 0) {
            s_ytDlpVersion = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        }
        s_ytDlpVersionState = 2;
        proc->deleteLater();
    });
    QObject::connect(proc, &QProcess::errorOccurred, proc, [proc](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart) {
            s_ytDlpVersionState = 2;
            proc->deleteLater();
        }
    });
    proc->start(exe, QStringList() << "--version");
}

bool VideoDownloader::isFfmpegAvailable() {
    return !findFfmpeg().isEmpty();
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
    m_runDestinations.clear();
    m_recentOutputLines.clear();
    m_finalPath.clear();
    m_stopRequested = false;
    // Random per run: a crafted title printed by yt-dlp cannot imitate the result-path line
    m_fileMarker = QStringLiteral("HBDM_FILE_%1:").arg(QRandomGenerator::global()->generate64(), 16, 16, QLatin1Char('0'));

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
        // Only when yt-dlp knows the height; a bare "[NAp]" otherwise
        qualityTag = "%(height& [{}p]|)s";
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
         << "--print" << QStringLiteral("after_move:%1%(filepath)s").arg(m_fileMarker)
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
    } else {
        qWarning("ffmpeg bulunamadı! Video ve ses birleştirme işlemleri başarısız olabilir.");
    }

    QString nodePath = findNode();
    const QString ver = ytDlpVersion(true);
    // --js-runtimes needs a modern yt-dlp; versions are "YYYY.MM.DD"
    if (!nodePath.isEmpty() && ver.left(4).toInt() >= 2024) {
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

void VideoDownloader::signalProcessGroup(int sig) {
    if (m_process == NULL) return;
    const qint64 pid = m_process->processId();
    if (pid <= 0) return;
    if (::kill(-static_cast<pid_t>(pid), sig) != 0) {
        // No own group (setpgid failed): signal the process itself
        ::kill(static_cast<pid_t>(pid), sig);
    }
}

void VideoDownloader::requestStop() {
    if (m_process == NULL || m_process->state() == QProcess::NotRunning || m_stopRequested) return;
    m_paused = true;
    m_stopRequested = true;
    signalProcessGroup(SIGTERM);
}

void VideoDownloader::stopProcess(bool cancelled) {
    if (m_process == NULL || m_process->state() == QProcess::NotRunning) return;
    if (cancelled) {
        m_cancelled = true;
    } else {
        m_paused = true;
    }
    if (!m_stopRequested) {
        m_stopRequested = true;
        signalProcessGroup(SIGTERM);
    }
    if (!m_process->waitForFinished(1500)) {
        signalProcessGroup(SIGKILL);
        m_process->waitForFinished(500);
    }
}

void VideoDownloader::pause() {
    stopProcess(false);
}

void VideoDownloader::cancel() {
    stopProcess(true);
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

    if (!m_fileMarker.isEmpty() && line.startsWith(m_fileMarker)) {
        QString candidate = line.mid(m_fileMarker.size()).trimmed();
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
                if (noteDestination(m_finalPath, true)) {
                    m_item.m_fileName = QFileInfo(m_finalPath).fileName();
                    emit atTitleDetected(m_item.m_fileName);
                }
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

    // A post-processor's output (e.g. the mp3 of "-x") is ours too while it is being written
    static QRegularExpression ppDestRegex(R"(^\[(?:ExtractAudio|VideoConvertor|VideoRemuxer)\] Destination:\s+(.+))");
    QRegularExpressionMatch ppMatch = ppDestRegex.match(line);
    if (ppMatch.hasMatch()) {
        addTempPath(ppMatch.captured(1).trimmed());
        return;
    }

    static QRegularExpression destRegex(R"(\[download\] Destination:\s+(.+))");
    QRegularExpressionMatch match = destRegex.match(line);
    if (match.hasMatch()) {
        m_finalPath = match.captured(1).trimmed();
        noteDestination(m_finalPath, true);
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
        noteDestination(m_finalPath, false);  // counts as a finished phase; not ours to delete
        m_item.m_fileName = QFileInfo(m_finalPath).fileName();
        emit atTitleDetected(m_item.m_fileName);
        return;
    }

    static QRegularExpression mergeRegex("\\[Merger\\] Merging formats into \"([^\"]+)\"");
    match = mergeRegex.match(line);
    if (match.hasMatch()) {
        m_finalPath = match.captured(1).trimmed();
        addTempPath(m_finalPath);  // an interrupted merge leaves "<name>.temp.<ext>"
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

void VideoDownloader::addTempPath(const QString &path) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (!abs.isEmpty() && PathUtils::isInsideDir(abs, m_item.m_directory) && !m_item.m_tempPaths.contains(abs)) {
        m_item.m_tempPaths.append(abs);
    }
}

bool VideoDownloader::noteDestination(const QString &path, bool ownFile) {
    const QString abs = QFileInfo(path).absoluteFilePath();
    if (abs.isEmpty()) return false;
    if (ownFile) {
        addTempPath(abs);
    }
    if (m_runDestinations.contains(abs)) return false;
    m_runDestinations.append(abs);
    // "bestvideo+bestaudio" downloads two files one after the other: the second one is the audio.
    // A single-file format only ever reports one path, whatever the extension.
    if (m_item.m_kind != DownloadKindAudio && m_runDestinations.size() >= 2) {
        m_isAudioPhase = true;
    }
    return true;
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
        if (!PathUtils::isInsideDir(m_finalPath, m_item.m_directory)) {
            emit atFinished(false, QString(), tr("İndirilen dosya indirme klasörünün dışında: %1").arg(m_finalPath));
        } else if (QFile::exists(m_finalPath)) {
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
