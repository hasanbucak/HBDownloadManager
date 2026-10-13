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
    QString m_etag;

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

    static const int s_maxParallelReplies = 6;
    static const int s_maxRetries = 5;
    static const int s_maxConnectFailures = 5;

    static bool writeFully(int fd, const char *data, qint64 size, off64_t offset, int *errOut = NULL);
    static bool validateContentRangeHeader(const QString &header,
                                           qint64 expectedStart,
                                           qint64 segEnd,
                                           qint64 totalBytes,
                                           qint64 *rangeFirstOut = NULL,
                                           qint64 *rangeLastOut = NULL);
    static QString uniqueTargetPath(const QString &baseDir, const QString &fileName, const QString &id = QString());

    // fdatasync the part file, then record the counters as durable (safe to persist)
    void syncToDisk();
    // Item whose segment/byte counters never claim data that is not yet on disk
    DownloadItem durableItem() const;

    // Test hook: shorten stall/connect/probe timeouts (milliseconds); 0 keeps the default
    static void setTimeoutsForTesting(qint64 stallMs, qint64 connectMs, qint64 probeMs);

signals:
    void atProbed(const ProbeResult &result);
    void atProgressUpdated(qint64 receivedBytes, qint64 totalBytes, double speed, const QList<SegmentInfo> &segments);
    void atFinished(bool success, const QString &errorMessage);

private slots:
    void onProbeMetaDataChanged();
    void onProbeFinished();
    void onSegmentMetaDataChanged();
    void onSegmentReadyRead();
    void onSegmentFinished();
    void onSingleMetaDataChanged();
    void onSingleReadyRead();
    void onSingleFinished();
    void onSpeedTimerTimeout();

private:
    void probe();
    void handleProbeReply(QNetworkReply *reply);
    void startMultiSegment();
    void launchPending();
    void startSingleStream();
    void launchSegment(int index);
    bool validateSegmentReply(QNetworkReply *reply, const SegmentInfo &seg);
    void scheduleRetry(int segIndex, const QString &reason, bool countAsRetry = true);
    void abortAllReplies();
    void resetDurable();
    void fallBackToSingleStream();
    void restartFromProbe(bool keepSegments);
    bool switchToOriginalUrl();
    void checkAllSegmentsCompleted();
    void finishDownload();
    void failDownload(const QString &error);

    bool openPartFile(bool preallocate);
    void closePartFile();
    void applyUniqueFilename();

    QNetworkRequest createRequest(const QUrl &url, const QString &rangeHeader = QString(), bool allowIfRange = true) const;
    static QString parseContentDisposition(const QString &header);

    DownloadItem m_item;
    QNetworkAccessManager *m_nam;
    QNetworkReply *m_probeReply;
    QNetworkReply *m_singleReply;
    QMap<QNetworkReply*, int> m_segmentReplies;

    QList<int> m_pendingSegments;
    QMap<QNetworkReply*, qint64> m_replyExpectedStart;
    QSet<QNetworkReply*> m_validatedReplies;
    QMap<QNetworkReply*, qint64> m_replyLaunchTime;
    QMap<QNetworkReply*, qint64> m_replyLastActivity;
    QSet<QNetworkReply*> m_repliesWithHeaders;

    bool m_singleValidated;
    qint64 m_singleLaunchTime;
    qint64 m_singleLastActivity;
    bool m_singleWithHeaders;
    int m_singleRetries;
    int m_singleConnectFailures;
    int m_fullRestarts;
    int m_probeRetries;
    qint64 m_probeLaunchTime;

    int m_fileFd;
    bool m_stopped;
    bool m_finished;

    QTimer *m_speedTimer;
    qint64 m_bytesInInterval;
    QElapsedTimer m_elapsedTimer;
    qint64 m_lastReportTime;

    QMap<int, int> m_segmentRetries;
    QMap<int, int> m_connectFailures;

    // Bumped on pause/fail/finish/restart; deferred lambdas from an older generation do nothing
    int m_generation;
    bool m_triedOriginalUrl;
    bool m_keepSegmentsOnProbe;
    QList<SegmentInfo> m_durableSegments;
    qint64 m_durableReceived;

    static qint64 s_stallMs;
    static qint64 s_connectMs;
    static qint64 s_probeMs;
};
