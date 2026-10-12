#include "ClipboardWatcher.h"
#include "MediaHosts.h"
#include "../core/DownloadManager.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QSet>

static QSet<QString> s_ignoredUrls;

void ClipboardWatcher::ignoreUrl(const QString &url) {
    s_ignoredUrls.insert(url.trimmed());
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
    if (text.isEmpty() || text == m_lastUrl) return;

    if (s_ignoredUrls.contains(text)) {
        s_ignoredUrls.remove(text);
        m_lastUrl = text;
        return;
    }

    QUrl url(text);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) return;

    if (isDownloadableUrl(url)) {
        m_lastUrl = text;
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
