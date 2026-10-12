#pragma once

#include "SegmentInfo.h"
#include <QString>
#include <QUrl>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>

enum DownloadKind {
    DownloadKindFile = 0,
    DownloadKindVideo,
    DownloadKindAudio
};

enum DownloadState {
    DownloadStateQueued = 0,
    DownloadStateConnecting,
    DownloadStateDownloading,
    DownloadStatePaused,
    DownloadStateCompleted,
    DownloadStateError,
    DownloadStateCancelled
};

enum FileCategory {
    FileCategoryAll = 0,
    FileCategoryImage,
    FileCategoryMusic,
    FileCategoryVideo,
    FileCategoryPrograms,
    FileCategoryDocument,
    FileCategoryCompressed,
    FileCategoryOther
};

class DownloadItem {
public:
    DownloadItem();
    explicit DownloadItem(const QUrl &url, const QString &directory = QString());

    QString m_id;
    QUrl m_url;
    QString m_fileName;
    QString m_directory;
    DownloadKind m_kind;
    DownloadState m_state;
    qint64 m_totalBytes;
    qint64 m_receivedBytes;
    bool m_supportsRanges;
    QList<SegmentInfo> m_segments;
    QMap<QString, QString> m_headers;
    QString m_cookieHeader;
    QJsonArray m_cookies;
    QString m_errorMessage;
    QDateTime m_createdAt;
    QDateTime m_completedAt;
    QString m_finalPath;
    int m_requestedSegments;
    QString m_quality;

    // Çalışma zamanı (non-persistent) alanlar
    double m_speed;
    double m_videoPercent;
    QString m_stage;
    qint64 m_lastReceived;
    qint64 m_lastSpeedTime;

    bool isActive() const;
    double progress() const;
    qint64 etaSeconds() const;
    QString stateString() const;
    QString formattedProgress() const;
    QString formattedSpeed() const;
    QString formattedEta() const;
    QString formattedSize() const;

    QString partFilePath() const;
    QString targetFilePath() const;

    FileCategory category() const;
    static QString categoryName(FileCategory cat);
    static FileCategory detectCategory(const QString &fileName);

    QJsonObject toJson() const;
    static DownloadItem fromJson(const QJsonObject &obj);
};
