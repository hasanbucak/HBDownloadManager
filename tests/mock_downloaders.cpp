#include "SegmentedDownloader.h"
#include "VideoDownloader.h"

// Minimal stubs for unit testing DownloadManager lifecycle
SegmentedDownloader::SegmentedDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent), m_item(item) {}

SegmentedDownloader::~SegmentedDownloader() {}

void SegmentedDownloader::start() {}
void SegmentedDownloader::pause() {}

void SegmentedDownloader::onProbeMetaDataChanged() {}
void SegmentedDownloader::onProbeFinished() {}
void SegmentedDownloader::onSegmentReadyRead() {}
void SegmentedDownloader::onSegmentFinished() {}
void SegmentedDownloader::onSingleReadyRead() {}
void SegmentedDownloader::onSingleFinished() {}
void SegmentedDownloader::onSpeedTimerTimeout() {}

VideoDownloader::VideoDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent), m_item(item) {}

VideoDownloader::~VideoDownloader() {}

void VideoDownloader::start() {}
void VideoDownloader::pause() {}

void VideoDownloader::onProcessReadyRead() {}
void VideoDownloader::onProcessFinished(int, QProcess::ExitStatus) {}

bool VideoDownloader::isAvailable() { return false; }
