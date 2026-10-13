#include "SegmentedDownloader.h"
#include "VideoDownloader.h"

// Minimal stubs for unit testing DownloadManager lifecycle
SegmentedDownloader::SegmentedDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent), m_item(item), m_nam(NULL), m_probeReply(NULL), m_singleReply(NULL) {}

SegmentedDownloader::~SegmentedDownloader() {}

void SegmentedDownloader::start() {}
void SegmentedDownloader::pause() {}
void SegmentedDownloader::cancel() {}
void SegmentedDownloader::syncToDisk() {}
DownloadItem SegmentedDownloader::durableItem() const { return m_item; }

void SegmentedDownloader::onProbeMetaDataChanged() {}
void SegmentedDownloader::onProbeFinished() {}
void SegmentedDownloader::onSegmentMetaDataChanged() {}
void SegmentedDownloader::onSegmentReadyRead() {}
void SegmentedDownloader::onSegmentFinished() {}
void SegmentedDownloader::onSingleMetaDataChanged() {}
void SegmentedDownloader::onSingleReadyRead() {}
void SegmentedDownloader::onSingleFinished() {}
void SegmentedDownloader::onSpeedTimerTimeout() {}

VideoDownloader::VideoDownloader(const DownloadItem &item, QObject *parent)
    : QObject(parent), m_item(item), m_process(NULL), m_tempCookieFile(NULL),
      m_cancelled(false), m_paused(false), m_stopRequested(false) {}

VideoDownloader::~VideoDownloader() {}

void VideoDownloader::start() {}
void VideoDownloader::pause() {}
void VideoDownloader::cancel() {}
void VideoDownloader::requestStop() {}

void VideoDownloader::onProcessReadyRead() {}
void VideoDownloader::onProcessFinished(int, QProcess::ExitStatus) {}

bool VideoDownloader::isAvailable() { return false; }
QString VideoDownloader::ytDlpVersion(bool) { return QString(); }
void VideoDownloader::prefetchYtDlpVersion() {}
bool VideoDownloader::isFfmpegAvailable() { return false; }
