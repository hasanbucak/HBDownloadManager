#include "ClipboardWatcher.h"
#include "MediaHosts.h"
#include "../core/DownloadManager.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QHash>
#include <QDateTime>

// URL -> expiry (ms since epoch). An entry that was never consumed goes stale after a short time.
static QHash<QString, qint64> s_ignoredUrls;
static QString s_lastUrl;
static const qint64 kIgnoreTtlMs = 5000;

void ClipboardWatcher::ignoreUrl(const QString &url) {
    s_ignoredUrls.insert(url.trimmed(), QDateTime::currentMSecsSinceEpoch() + kIgnoreTtlMs);
}

void ClipboardWatcher::resetLastUrl() {
    s_lastUrl.clear();
}

ClipboardWatcher::ClipboardWatcher(QObject *parent)
    : QObject(parent),
      m_clipboard(QGuiApplication::clipboard()),
      m_enabled(true) {
    if (m_clipboard != NULL) {
        connect(m_clipboard, SIGNAL(dataChanged()), this, SLOT(onClipboardChanged()));
    }
}

ClipboardWatcher::~ClipboardWatcher() {
}

void ClipboardWatcher::onClipboardChanged() {
    if (!m_enabled || !DownloadManager::instance().clipboardMonitoringEnabled() || m_clipboard == NULL) return;

    QString text = m_clipboard->text().trimmed();
    if (text.isEmpty() || text == s_lastUrl) return;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QHash<QString, qint64>::iterator ig = s_ignoredUrls.find(text);
    if (ig != s_ignoredUrls.end()) {
        const bool fresh = ig.value() >= now;
        s_ignoredUrls.erase(ig);
        if (fresh) {
            s_lastUrl = text;
            return;
        }
    }

    QUrl url(text);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) return;

    if (isDownloadableUrl(url)) {
        s_lastUrl = text;
        emit atUrlDetected(url);
    }
}

bool ClipboardWatcher::isDownloadableUrl(const QUrl &url) const {
    if (!DownloadManager::instance().clipboardMonitoringEnabled()) return false;

    QString path = url.path().toLower();
    QString ext = QFileInfo(path).suffix();

    if (!ext.isEmpty()) {
        QStringList exts = DownloadManager::instance().captureExtensions();
        if (exts.contains(ext)) {
            return true;
        }
    }

    if (MediaHosts::isStreamingSite(url.host())) {
        return true;
    }

    return false;
}
