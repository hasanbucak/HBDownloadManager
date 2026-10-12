#include "ClipboardWatcher.h"
#include "../core/DownloadManager.h"
#include <QGuiApplication>
#include <QFileInfo>
#include <QRegularExpression>

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
    if (!m_enabled || m_clipboard == NULL) return;

    QString text = m_clipboard->text().trimmed();
    if (text.isEmpty() || text == m_lastUrl) return;

    QUrl url(text);
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https")) return;

    if (isDownloadableUrl(url)) {
        m_lastUrl = text;
        emit atUrlDetected(url);
    }
}

bool ClipboardWatcher::isDownloadableUrl(const QUrl &url) const {
    QString path = url.path().toLower();
    QString ext = QFileInfo(path).suffix();

    if (!ext.isEmpty()) {
        QStringList exts = DownloadManager::instance().captureExtensions();
        if (exts.contains(ext)) {
            return true;
        }
    }

    // Popüler video platformları
    QString host = url.host().toLower();
    if (host.contains("youtube.com") || host.contains("youtu.be") ||
        host.contains("vimeo.com") || host.contains("dailymotion.com") ||
        host.contains("tiktok.com") || host.contains("instagram.com") ||
        host.contains("facebook.com") || host.contains("fb.watch") || host.contains("fb.com") ||
        host.contains("twitter.com") || host.contains("x.com")) {
        return true;
    }

    return false;
}
