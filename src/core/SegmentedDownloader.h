#pragma once

#include "DownloadItem.h"
#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QMap>

struct ProbeResult {
    qint64 m_totalBytes;
    bool m_supportsRanges;
    QString m_suggestedFileName;
    QString m_contentType;
    QUrl m_finalUrl;

    ProbeResult()
        : m_totalBytes(-1), m_supportsRanges(false) {}
};

class SegmentedDownloader : public QObject {
    Q_OBJECT
public:
    explicit SegmentedDownloader(const DownloadItem &item, QObject *parent = NULL);
    virtual ~SegmentedDownloader();

    DownloadItem item() const { return m_item; }

    void start();
    void pause();
    void cancel();

signals:
    void atProbed(const ProbeResult &result);
    void atProgressUpdated(qint64 receivedBytes, qint64 totalBytes, double speed, const QList<SegmentInfo> &segments);
    void atFinished(bool success, const QString &errorMessage);

private slots:
    void onProbeFinished();
    void onSegmentReadyRead();
    void onSegmentFinished();
    void onSingleReadyRead();
    void onSingleFinished();
    void onSpeedTimerTimeout();
    void onRetryTimerTimeout();

private:
    void probe();
    void handleProbeReply(QNetworkReply *reply);
    void startMultiSegment();
    void startSingleStream();
    void launchSegment(int index);
    void checkAllSegmentsCompleted();
    void finishDownload();
    void failDownload(const QString &error);

    bool openPartFile(bool preallocate);
    void closePartFile();
    void applyUniqueFilename();

    QNetworkRequest createRequest(const QUrl &url, const QString &rangeHeader = QString()) const;
    static QString parseContentDisposition(const QString &header);

    DownloadItem m_item;
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_probeReply;
    QNetworkReply *m_singleReply;
    QMap<QNetworkReply*, int> m_segmentReplies;

    int m_fileFd;
    bool m_stopped;
    bool m_finished;

    QTimer *m_speedTimer;
    QTimer *m_retryTimer;
    qint64 m_bytesInInterval;
    QElapsedTimer m_elapsedTimer;
    qint64 m_lastReportTime;

    static const int s_maxRetries = 5;
    QMap<int, int> m_segmentRetries;
    QList<int> m_pendingRetries;
};
