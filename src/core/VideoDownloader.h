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

    static QString findYtDlp();
    static QString findFfmpeg();
    static QString findNode();
    static bool isAvailable();

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

    DownloadItem m_item;
    QProcess *m_process;
    QString m_finalPath;
    QTemporaryFile *m_tempCookieFile;
    bool m_cancelled;
    bool m_paused;
    qint64 m_videoTotalBytes;
    qint64 m_audioTotalBytes;
    bool m_isAudioPhase;
};
