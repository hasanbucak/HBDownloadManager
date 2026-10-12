#include "TrayManager.h"
#include "MainWindow.h"
#include "NewDownloadDialog.h"
#include "../core/DownloadManager.h"
#include <QApplication>
#include <QIcon>
#include <QMessageBox>

TrayManager::TrayManager(MainWindow *mainWindow, QObject *parent)
    : QObject(parent),
      m_mainWindow(mainWindow),
      m_trayIcon(new QSystemTrayIcon(this)),
      m_menu(new QMenu()),
      m_actShowHide(NULL),
      m_actNew(NULL),
      m_actPauseAll(NULL),
      m_actResumeAll(NULL),
      m_actQuit(NULL) {
    QIcon icon(":/icons/app_icon.png");
    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_ArrowDown);
    }
    m_trayIcon->setIcon(icon);
    m_trayIcon->setToolTip(tr("HB Download Manager (Linux)"));

    setupMenu();

    connect(m_trayIcon, SIGNAL(activated(QSystemTrayIcon::ActivationReason)),
            this, SLOT(onTrayIconActivated(QSystemTrayIcon::ActivationReason)));

    connect(&DownloadManager::instance(), SIGNAL(atItemCompleted(DownloadItem)),
            this, SLOT(onItemCompleted(DownloadItem)));
    connect(&DownloadManager::instance(), SIGNAL(atItemFailed(DownloadItem,QString)),
            this, SLOT(onItemFailed(DownloadItem,QString)));
}

TrayManager::~TrayManager() {
    delete m_menu;
}

void TrayManager::setupMenu() {
    m_actShowHide = m_menu->addAction(tr("Göster / Gizle"), this, SLOT(onShowHideClicked()));
    m_actNew = m_menu->addAction(tr("Yeni İndirme [+]"), this, SLOT(onNewDownloadClicked()));
    m_menu->addSeparator();
    m_actPauseAll = m_menu->addAction(tr("Tümünü Duraklat"), this, SLOT(onPauseAllClicked()));
    m_actResumeAll = m_menu->addAction(tr("Tümünü Başlat"), this, SLOT(onResumeAllClicked()));
    m_menu->addSeparator();
    m_actQuit = m_menu->addAction(tr("Çıkış"), this, SLOT(onQuitClicked()));

    m_trayIcon->setContextMenu(m_menu);
}

void TrayManager::show() {
    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        m_trayIcon->show();
    }
}

void TrayManager::onTrayIconActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        onShowHideClicked();
    }
}

void TrayManager::onShowHideClicked() {
    if (m_mainWindow == NULL) return;

    if (m_mainWindow->isVisible() && !m_mainWindow->isMinimized()) {
        m_mainWindow->hide();
    } else {
        m_mainWindow->showNormal();
        m_mainWindow->activateWindow();
        m_mainWindow->raise();
    }
}

void TrayManager::onNewDownloadClicked() {
    NewDownloadDialog dlg(QUrl(), m_mainWindow);
    if (dlg.exec() == QDialog::Accepted) {
        DownloadManager::instance().addDownload(dlg.downloadItem(), dlg.shouldStartImmediately());
    }
}

void TrayManager::onPauseAllClicked() {
    DownloadManager::instance().pauseAll();
}

void TrayManager::onResumeAllClicked() {
    const QList<DownloadItem> &list = DownloadManager::instance().items();
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).m_state == DownloadStatePaused) {
            DownloadManager::instance().resumeDownload(list.at(i).m_id);
        }
    }
}

void TrayManager::onQuitClicked() {
    if (DownloadManager::instance().activeCount() > 0) {
        int ret = QMessageBox::question(
            m_mainWindow, tr("Çıkış Onayı"),
            tr("Aktif indirmeler devam ediyor. Uygulamadan çıkmak istediğinizden emin misiniz?"),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (ret != QMessageBox::Yes) {
            return;
        }
    }
    DownloadManager::instance().pauseAll();
    QApplication::quit();
}

void TrayManager::onItemCompleted(const DownloadItem &item) {
    if (QSystemTrayIcon::isSystemTrayAvailable() && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(
            tr("İndirme Tamamlandı"),
            item.m_fileName,
            QSystemTrayIcon::Information,
            4000
        );
    }
}

void TrayManager::onItemFailed(const DownloadItem &item, const QString &error) {
    if (QSystemTrayIcon::isSystemTrayAvailable() && m_trayIcon->isVisible()) {
        m_trayIcon->showMessage(
            tr("İndirme Hatası"),
            QString("%1: %2").arg(item.m_fileName, error),
            QSystemTrayIcon::Warning,
            5000
        );
    }
}
