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
#include <QSet>

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

    static bool writeFully(int fd, const char *data, qint64 size, off64_t offset, int *errOut = NULL);
    static bool validateContentRangeHeader(const QString &header,
                                           qint64 expectedStart,
                                           qint64 segEnd,
                                           qint64 totalBytes,
                                           qint64 *rangeFirstOut = NULL,
                                           qint64 *rangeLastOut = NULL);

signals:
    void atProbed(const ProbeResult &result);
    void atProgressUpdated(qint64 receivedBytes, qint64 totalBytes, double speed, const QList<SegmentInfo> &segments);
    void atFinished(bool success, const QString &errorMessage);

private slots:
    void onProbeMetaDataChanged();
    void onProbeFinished();
    void onSegmentReadyRead();
    void onSegmentFinished();
    void onSingleReadyRead();
    void onSingleFinished();
    void onSpeedTimerTimeout();

private:
    void probe();
    void handleProbeReply(QNetworkReply *reply);
    void startMultiSegment();
    void startSingleStream();
    void launchSegment(int index);
    bool validateSegmentReply(QNetworkReply *reply, const SegmentInfo &seg);
    void scheduleRetry(int segIndex, const QString &reason);
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

    QMap<QNetworkReply*, qint64> m_replyExpectedStart;
    QSet<QNetworkReply*> m_validatedReplies;
    QMap<QNetworkReply*, qint64> m_replyLastActivity;

    bool m_singleValidated;
    qint64 m_singleLastActivity;
    int m_singleRetries;

    int m_fileFd;
    bool m_stopped;
    bool m_finished;

    QTimer *m_speedTimer;
    qint64 m_bytesInInterval;
    QElapsedTimer m_elapsedTimer;
    qint64 m_lastReportTime;

    static const int s_maxRetries = 5;
    QMap<int, int> m_segmentRetries;
};
