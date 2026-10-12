#pragma once

#include "../core/DownloadItem.h"
#include <QObject>
#include <QSystemTrayIcon>
#include <QMenu>

class MainWindow;

class TrayManager : public QObject {
    Q_OBJECT
public:
    explicit TrayManager(MainWindow *mainWindow, QObject *parent = NULL);
    virtual ~TrayManager();

    void show();

signals:
    void atShowRequested();
    void atQuitRequested();

private slots:
    void onTrayIconActivated(QSystemTrayIcon::ActivationReason reason);
    void onShowHideClicked();
    void onNewDownloadClicked();
    void onPauseAllClicked();
    void onResumeAllClicked();
    void onQuitClicked();

    void onItemCompleted(const DownloadItem &item);
    void onItemFailed(const DownloadItem &item, const QString &error);

private:
    void setupMenu();

    MainWindow *m_mainWindow;
    QSystemTrayIcon *m_trayIcon;
    QMenu *m_menu;
    QAction *m_actShowHide;
    QAction *m_actNew;
    QAction *m_actPauseAll;
    QAction *m_actResumeAll;
    QAction *m_actQuit;
};
