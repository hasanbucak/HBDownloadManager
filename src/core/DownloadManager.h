#pragma once

#include "DownloadItem.h"
#include "SegmentedDownloader.h"
#include "VideoDownloader.h"
#include <QObject>
#include <QSet>
#include <QList>
#include <QMap>

class QTimer;

class DownloadManager : public QObject {
    Q_OBJECT
public:
    static DownloadManager& instance();

    const QList<DownloadItem>& items() const { return m_items; }
    int activeCount() const;
    DownloadItem* findItem(const QString &id);

    void initialize();

    QString addDownload(const DownloadItem &item, bool startImmediately = true);
    void pauseDownload(const QString &id);
    void resumeDownload(const QString &id);
    void cancelDownload(const QString &id);
    void removeDownload(const QString &id, bool deleteFile = false);

    QString defaultDirectory() const;
    void setDefaultDirectory(const QString &dir);
    int defaultSegments() const;
    void setDefaultSegments(int count);
    int maxConcurrentDownloads() const;
    void setMaxConcurrentDownloads(int count);
    quint16 serverPort() const;
    void setServerPort(quint16 port);
    bool clipboardMonitoringEnabled() const;
    void setClipboardMonitoringEnabled(bool enabled);
    QStringList captureExtensions() const;

    void loadState();

public slots:
    void saveState();
    void shutdown();
    void pauseAll(bool immediateDelete = false);

signals:
    void atItemAdded(const DownloadItem &item);
    void atItemUpdated(const DownloadItem &item);
    void atItemRemoved(const QString &id);
    void atItemCompleted(const DownloadItem &item);
    void atItemFailed(const DownloadItem &item, const QString &errorMessage);

private slots:
    void onPeriodicSaveTimeout();

    void onSegmentedProbed(const ProbeResult &res);
    void onSegmentedProgressUpdated(qint64 received, qint64 total, double speed, const QList<SegmentInfo> &segs);
    void onSegmentedFinished(bool success, const QString &errMsg);

    void onVideoTitleDetected(const QString &title);
    void onVideoProgressUpdated(double pct, qint64 totalBytes, qint64 receivedBytes, const QString &speedStr, const QString &etaStr, const QString &stage);
    void onVideoFinished(bool success, const QString &finalPath, const QString &errMsg);

private:
    explicit DownloadManager(QObject *parent = NULL);
    virtual ~DownloadManager();

    void processQueue();
    void startItem(DownloadItem *item);
    void stopDownloader(const QString &id, bool immediateDelete = false);
    void cleanupOrphanParts();
    void syncVideoTempPaths(DownloadItem *it, QObject *downloader);
    static void removeVideoLeftovers(const QStringList &recorded, const QString &directory,
                                     const QSet<QString> &inUse);
    QString configFilePath() const;
    QString findIdByDownloader(QObject *downloader) const;

    QList<DownloadItem> m_items;
    QMap<QString, SegmentedDownloader*> m_activeSegmented;
    QMap<QString, VideoDownloader*> m_activeVideo;

    QTimer *m_saveTimer;
    bool m_isShuttingDown;

    int m_maxConcurrent;
    int m_defaultSegments;
    quint16 m_serverPort;
    bool m_clipboardMonitoring;
    QString m_defaultDir;
    QStringList m_captureExtensions;
};
