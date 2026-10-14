#pragma once

#include "DownloadItem.h"
#include <QObject>
#include <QProcess>
#include <QTemporaryFile>

class VideoDownloader : public QObject {
    Q_OBJECT
public:
    explicit VideoDownloader(const DownloadItem &item, QObject *parent = NULL);
    virtual ~VideoDownloader();

    DownloadItem item() const { return m_item; }
    void start();
    void pause();
    void cancel();
    // Sends SIGTERM to the yt-dlp process group without waiting (pause()/cancel() then wait)
    void requestStop();

    static QString findYtDlp();
    static QString findFfmpeg();
    static QString findNode();
    static bool isAvailable();
    // Cached version; blocks (up to 2 s) only when allowBlocking and the async probe has not finished
    static QString ytDlpVersion(bool allowBlocking = false);
    // Starts the asynchronous "yt-dlp --version" probe (call once at start-up)
    static void prefetchYtDlpVersion();
    static bool isFfmpegAvailable();

signals:
    void atTitleDetected(const QString &title);
    void atProgressUpdated(double percent, qint64 totalBytes, qint64 receivedBytes, const QString &speedStr, const QString &etaStr, const QString &stage);
    void atFinished(bool success, const QString &finalPath, const QString &errorMessage);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    void parseOutputLine(const QString &line);
    QString createCookieFile();
    void signalProcessGroup(int sig);
    void stopProcess(bool cancelled);
    // Records a file yt-dlp writes; returns true when it is a new one for this run
    bool noteDestination(const QString &path, bool ownFile);
    void addTempPath(const QString &path);

    DownloadItem m_item;
    QProcess *m_process;
    QString m_finalPath;
    QTemporaryFile *m_tempCookieFile;
    bool m_cancelled;
    bool m_paused;
    qint64 m_videoTotalBytes;
    qint64 m_audioTotalBytes;
    bool m_isAudioPhase;
    QStringList m_runDestinations;
    QStringList m_recentOutputLines;
    QString m_fileMarker;
    bool m_stopRequested;
};
