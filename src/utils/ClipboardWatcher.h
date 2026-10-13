#pragma once

#include <QObject>
#include <QUrl>
#include <QClipboard>

class ClipboardWatcher : public QObject {
    Q_OBJECT
public:
    explicit ClipboardWatcher(QObject *parent = NULL);
    virtual ~ClipboardWatcher();

    bool isEnabled() const { return m_enabled; }
    void setEnabled(bool enabled) { m_enabled = enabled; }
    static void ignoreUrl(const QString &url);
    // Forget the last seen clipboard URL (call when its dialog closed) so it can be offered again
    static void resetLastUrl();

signals:
    void atUrlDetected(const QUrl &url);

private slots:
    void onClipboardChanged();

private:
    bool isDownloadableUrl(const QUrl &url) const;

    QClipboard *m_clipboard;
    bool m_enabled;
};
