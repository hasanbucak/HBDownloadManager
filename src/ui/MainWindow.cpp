#include "MainWindow.h"
#include "NewDownloadDialog.h"
#include "DownloadDetailDialog.h"
#include "../core/DownloadManager.h"
#include "../utils/ClipboardWatcher.h"
#include <QSplitter>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QStatusBar>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QDesktopServices>
#include <QProgressBar>
#include <QFileInfo>
#include <QCloseEvent>
#include <QInputDialog>
#include <QProcess>
#include <QApplication>
#include <QSystemTrayIcon>
#include <QClipboard>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent),
      m_toolBar(NULL),
      m_actNew(NULL),
      m_actPause(NULL),
      m_actResume(NULL),
      m_actRemove(NULL),
      m_actClearCompleted(NULL),
      m_actSettings(NULL),
      m_actQuit(NULL),
      m_categoryList(new QListWidget(this)),
      m_downloadTable(new QTableWidget(this)),
      m_statusSpeedLabel(new QLabel(this)),
      m_statusActiveLabel(new QLabel(this)),
      m_statusServerLabel(new QLabel(this)),
      m_selectedCategory(0) {
    setupUi();
    applyDarkTheme();
    refreshTable();

    connect(&DownloadManager::instance(), SIGNAL(atItemAdded(DownloadItem)),
            this, SLOT(onItemAdded(DownloadItem)));
    connect(&DownloadManager::instance(), SIGNAL(atItemUpdated(DownloadItem)),
            this, SLOT(onItemUpdated(DownloadItem)));
    connect(&DownloadManager::instance(), SIGNAL(atItemRemoved(QString)),
            this, SLOT(onItemRemoved(QString)));
}

MainWindow::~MainWindow() {
}

void MainWindow::setServerStatus(bool listening, quint16 port, const QString &errorMsg) {
    if (listening) {
        m_statusServerLabel->setText(tr("Köprü: 127.0.0.1:%1 (Aktif)").arg(port));
        m_statusServerLabel->setStyleSheet("");
    } else {
        m_statusServerLabel->setText(tr("Köprü Hatası: %1").arg(errorMsg.isEmpty() ? tr("Başlatılamadı") : errorMsg));
        m_statusServerLabel->setStyleSheet("color: #ff6b6b;");
    }
}

void MainWindow::setupUi() {
    setWindowTitle(tr("HB Download Manager (Linux)"));
    resize(960, 580);

    setupMenuBar();
    setupToolBar();
    setupSidebar();
    setupTable();
    setupStatusBar();

    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);
    splitter->addWidget(m_categoryList);
    splitter->addWidget(m_downloadTable);
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 4);

    setCentralWidget(splitter);
}

void MainWindow::setupMenuBar() {
    QMenu *fileMenu = menuBar()->addMenu(tr("&Dosya"));
    fileMenu->addAction(tr("&Yeni İndirme..."), this, SLOT(onNewDownloadClicked()), QKeySequence::New);
    fileMenu->addSeparator();
    m_actQuit = fileMenu->addAction(tr("&Çıkış"), this, SLOT(onQuitTriggered()), QKeySequence(Qt::CTRL | Qt::Key_Q));
}

void MainWindow::setupToolBar() {
    m_toolBar = addToolBar(tr("Ana Araç Çubuğu"));
    m_toolBar->setMovable(false);
    m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    m_actNew = m_toolBar->addAction(tr("Yeni İndirme [+]"), this, SLOT(onNewDownloadClicked()));
    m_actPause = m_toolBar->addAction(tr("Duraklat [||]"), this, SLOT(onPauseClicked()));
    m_actResume = m_toolBar->addAction(tr("Devam Et [▶]"), this, SLOT(onResumeClicked()));
    m_actRemove = m_toolBar->addAction(tr("Sil [X]"), this, SLOT(onRemoveClicked()));
    m_toolBar->addSeparator();
    m_actClearCompleted = m_toolBar->addAction(tr("Tamamlananları Temizle"), this, SLOT(onClearCompletedClicked()));
    m_actSettings = m_toolBar->addAction(tr("Ayarlar / Eklentiler [⚙]"), this, SLOT(onSettingsClicked()));
}

void MainWindow::setupSidebar() {
    m_categoryList->setFixedWidth(200);

    m_categoryList->addItem(tr("📁 Tümü"));
    m_categoryList->addItem(tr("⬇️ Devam Edenler"));
    m_categoryList->addItem(tr("⏸️ Duraklatılanlar"));
    m_categoryList->addItem(tr("✅ Tamamlananlar"));
    m_categoryList->addItem(tr("🖼️ Resimler"));
    m_categoryList->addItem(tr("🎵 Müzik"));
    m_categoryList->addItem(tr("🎬 Videolar"));
    m_categoryList->addItem(tr("📦 Programlar"));
    m_categoryList->addItem(tr("📄 Belgeler"));
    m_categoryList->addItem(tr("🗜️ Sıkıştırılmış"));

    m_categoryList->setCurrentRow(0);
    connect(m_categoryList, SIGNAL(currentRowChanged(int)), this, SLOT(onCategorySelected(int)));
}

void MainWindow::setupTable() {
    m_downloadTable->setColumnCount(7);
    QStringList headers;
    headers << tr("Dosya Adı") << tr("Boyut") << tr("İlerleme")
            << tr("Hız") << tr("Kalan Süre") << tr("Durum") << tr("Eklenme Tarihi");
    m_downloadTable->setHorizontalHeaderLabels(headers);

    m_downloadTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_downloadTable->setColumnWidth(2, 140);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);

    m_downloadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_downloadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_downloadTable->setContextMenuPolicy(Qt::CustomContextMenu);

    connect(m_downloadTable, SIGNAL(cellDoubleClicked(int,int)), this, SLOT(onTableDoubleClicked(int,int)));
    connect(m_downloadTable, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(onTableContextMenu(QPoint)));
}

void MainWindow::setupStatusBar() {
    statusBar()->addWidget(m_statusActiveLabel);
    statusBar()->addPermanentWidget(m_statusSpeedLabel);
    statusBar()->addPermanentWidget(m_statusServerLabel);

    m_statusActiveLabel->setText(tr("0 Aktif İndirme"));
    m_statusSpeedLabel->setText(tr("Toplam Hız: 0 B/s"));
    m_statusServerLabel->setText(tr("Köprü: 127.0.0.1:48213 (Aktif)"));
}

void MainWindow::applyDarkTheme() {
    setStyleSheet(
        "QMainWindow { background-color: #1e1e1e; color: #ffffff; }"
        "QMenuBar { background-color: #252526; color: #cccccc; border-bottom: 1px solid #3c3c3c; }"
        "QMenuBar::item { background-color: transparent; padding: 4px 8px; }"
        "QMenuBar::item:selected { background-color: #094771; color: #ffffff; }"
        "QMenu { background-color: #252526; color: #ffffff; border: 1px solid #3c3c3c; }"
        "QMenu::item { padding: 6px 20px; }"
        "QMenu::item:selected { background-color: #094771; }"
        "QMenu::separator { height: 1px; background: #3c3c3c; margin: 4px 0; }"
        "QToolBar { background-color: #252526; border-bottom: 1px solid #3c3c3c; padding: 4px; spacing: 6px; }"
        "QToolButton { background-color: #333333; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 6px 12px; font-weight: bold; }"
        "QToolButton:hover { background-color: #444444; }"
        "QToolButton:pressed { background-color: #222222; }"
        "QListWidget { background-color: #252526; color: #cccccc; border: none; font-size: 13px; }"
        "QListWidget::item { padding: 8px 12px; border-radius: 4px; }"
        "QListWidget::item:selected { background-color: #094771; color: #ffffff; }"
        "QListWidget::item:hover:!selected { background-color: #2a2d2e; }"
        "QTableWidget { background-color: #1e1e1e; color: #d4d4d4; gridline-color: #2d2d2d; border: none; selection-background-color: #094771; }"
        "QHeaderView::section { background-color: #252526; color: #cccccc; padding: 6px; border: 1px solid #2d2d2d; font-weight: bold; }"
        "QStatusBar { background-color: #007acc; color: #ffffff; font-weight: 500; }"
        "QDialog { background-color: #1e1e1e; color: #ffffff; }"
        "QLabel { color: #ffffff; }"
        "QLineEdit { background-color: #2d2d2d; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 5px; }"
        "QComboBox { background-color: #2d2d2d; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 4px; }"
        "QPushButton { background-color: #0e639c; color: #ffffff; border: none; border-radius: 4px; padding: 6px 14px; font-weight: bold; }"
        "QPushButton:hover { background-color: #1177bb; }"
        "QPushButton:pressed { background-color: #094771; }"
    );
}

void MainWindow::onCategorySelected(int row) {
    m_selectedCategory = row;
    refreshTable();
}

void MainWindow::refreshTable() {
    m_downloadTable->setRowCount(0);
    const QList<DownloadItem> &items = DownloadManager::instance().items();

    for (int i = 0; i < items.size(); ++i) {
        const DownloadItem &it = items.at(i);
        bool match = false;

        switch (m_selectedCategory) {
        case 0: match = true; break; // Tümü
        case 1: match = it.isActive(); break; // Devam Edenler
        case 2: match = (it.m_state == DownloadStatePaused); break; // Duraklatılanlar
        case 3: match = (it.m_state == DownloadStateCompleted); break; // Tamamlananlar
        case 4: match = (it.category() == FileCategoryImage); break;
        case 5: match = (it.category() == FileCategoryMusic); break;
        case 6: match = (it.category() == FileCategoryVideo); break;
        case 7: match = (it.category() == FileCategoryPrograms); break;
        case 8: match = (it.category() == FileCategoryDocument); break;
        case 9: match = (it.category() == FileCategoryCompressed); break;
        default: match = true; break;
        }

        if (match) {
            int row = m_downloadTable->rowCount();
            m_downloadTable->insertRow(row);
            updateTableRow(row, it);
        }
    }
}

void MainWindow::updateTableRow(int row, const DownloadItem &item) {
    QTableWidgetItem *item0 = new QTableWidgetItem(item.m_fileName);
    item0->setData(Qt::UserRole, item.m_id);
    m_downloadTable->setItem(row, 0, item0);

    m_downloadTable->setItem(row, 1, new QTableWidgetItem(item.formattedSize()));

    // İlerleme Çubuğu Widget'ı
    QProgressBar *pb = qobject_cast<QProgressBar*>(m_downloadTable->cellWidget(row, 2));
    if (pb == NULL) {
        pb = new QProgressBar(m_downloadTable);
        pb->setRange(0, 100);
        pb->setTextVisible(true);
        pb->setStyleSheet("QProgressBar { border: 1px solid #444; border-radius: 3px; text-align: center; height: 16px; background: #222; color: #fff; font-size: 11px; } "
                          "QProgressBar::chunk { background: #4CAF50; border-radius: 2px; }");
        m_downloadTable->setCellWidget(row, 2, pb);
    }
    pb->setValue(static_cast<int>(item.progress() * 100.0));

    m_downloadTable->setItem(row, 3, new QTableWidgetItem(item.formattedSpeed()));
    m_downloadTable->setItem(row, 4, new QTableWidgetItem(item.formattedEta()));
    m_downloadTable->setItem(row, 5, new QTableWidgetItem(item.stateString()));
    QString dateStr = item.m_createdAt.isValid()
        ? item.m_createdAt.toString("dd.MM.yyyy hh:mm")
        : QDateTime::currentDateTime().toString("dd.MM.yyyy hh:mm");
    m_downloadTable->setItem(row, 6, new QTableWidgetItem(dateStr));
}

int MainWindow::findRowById(const QString &id) const {
    for (int r = 0; r < m_downloadTable->rowCount(); ++r) {
        QTableWidgetItem *it = m_downloadTable->item(r, 0);
        if (it != NULL && it->data(Qt::UserRole).toString() == id) {
            return r;
        }
    }
    return -1;
}

QString MainWindow::selectedDownloadId() const {
    int row = m_downloadTable->currentRow();
    if (row < 0) return QString();
    QTableWidgetItem *it = m_downloadTable->item(row, 0);
    return it != NULL ? it->data(Qt::UserRole).toString() : QString();
}

void MainWindow::onNewDownloadClicked() {
    NewDownloadDialog dlg(QUrl(), this);
    if (dlg.exec() == QDialog::Accepted) {
        DownloadManager::instance().addDownload(dlg.downloadItem(), dlg.shouldStartImmediately());
    }
}

void MainWindow::onPauseClicked() {
    QString id = selectedDownloadId();
    if (!id.isEmpty()) {
        DownloadManager::instance().pauseDownload(id);
    }
}

void MainWindow::onResumeClicked() {
    QString id = selectedDownloadId();
    if (!id.isEmpty()) {
        DownloadManager::instance().resumeDownload(id);
    }
}

void MainWindow::onRemoveClicked() {
    QString id = selectedDownloadId();
    if (id.isEmpty()) return;

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("İndirmeyi Kaldır"));
    msgBox.setText(tr("Seçili indirme listeden kaldırılsın mı?"));
    msgBox.setInformativeText(tr("Dosyayı sabit diskinizden de tamamen silmek istiyor musunuz?"));
    msgBox.setIcon(QMessageBox::Question);

    QPushButton *btnRemoveOnly = msgBox.addButton(tr("Listeden Kaldır"), QMessageBox::ActionRole);
    QPushButton *btnDeleteFile = msgBox.addButton(tr("Dosyayı da Sil"), QMessageBox::DestructiveRole);
    QPushButton *btnCancel = msgBox.addButton(tr("İptal"), QMessageBox::RejectRole);
    Q_UNUSED(btnCancel);

    msgBox.setDefaultButton(btnRemoveOnly);
    msgBox.exec();

    if (msgBox.clickedButton() == btnDeleteFile) {
        DownloadManager::instance().removeDownload(id, true);
    } else if (msgBox.clickedButton() == btnRemoveOnly) {
        DownloadManager::instance().removeDownload(id, false);
    }
}

void MainWindow::onClearCompletedClicked() {
    const QList<DownloadItem> &list = DownloadManager::instance().items();
    QStringList toRemove;
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).m_state == DownloadStateCompleted ||
            list.at(i).m_state == DownloadStateError ||
            list.at(i).m_state == DownloadStateCancelled) {
            toRemove.append(list.at(i).m_id);
        }
    }

    for (int i = 0; i < toRemove.size(); ++i) {
        DownloadManager::instance().removeDownload(toRemove.at(i), false);
    }
}

void MainWindow::onSettingsClicked() {
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Ayarlar ve Tarayıcı Entegrasyonu"));
    dlg.resize(480, 240);

    QVBoxLayout *layout = new QVBoxLayout(&dlg);
    QLabel *infoLabel = new QLabel(tr("<b>Tarayıcı Entegrasyonu (Chrome, Opera, Brave, Edge, Firefox, Chromium)</b><br>"
                                      "Tarayıcılardan indirmelerin otomatik yakalanması için sistem native host kayıtlarını oluşturun:"), &dlg);
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    QPushButton *btnInstallHost = new QPushButton(tr("Tarayıcı Eklenti Köprüsünü Kur (1-Tık Kurulum)"), &dlg);
    layout->addWidget(btnInstallHost);
    connect(btnInstallHost, SIGNAL(clicked()), this, SLOT(onInstallHostBridgeClicked()));

    layout->addSpacing(10);
    QPushButton *btnClose = new QPushButton(tr("Kapat"), &dlg);
    connect(btnClose, SIGNAL(clicked()), &dlg, SLOT(accept()));
    layout->addWidget(btnClose);

    dlg.exec();
}

void MainWindow::onInstallHostBridgeClicked() {
    QString hostExe = QApplication::applicationDirPath() + "/hbdm-native-host";
    if (QFile::exists(hostExe)) {
        QProcess::execute(hostExe, QStringList() << "--install");
        QMessageBox::information(this, tr("Başarılı"), tr("Tarayıcı native host kayıtları başarıyla yüklendi!\nChrome, Opera, Firefox, Brave ve Edge eklentileri artık hazır."));
    } else {
        QMessageBox::warning(this, tr("Uyarı"), tr("Native host ikilisi bulunamadı: %1").arg(hostExe));
    }
}

void MainWindow::onTableDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    if (row < 0) return;
    QTableWidgetItem *it = m_downloadTable->item(row, 0);
    if (it == NULL) return;

    QString id = it->data(Qt::UserRole).toString();
    DownloadItem *item = DownloadManager::instance().findItem(id);
    if (item == NULL) return;

    if (item->m_state == DownloadStateCompleted) {
        // Tamamlanmışsa dosyayı aç
        QDesktopServices::openUrl(QUrl::fromLocalFile(item->targetFilePath()));
    } else {
        // İndiriliyorsa veya duraklatılmışsa detay penceresini aç
        DownloadDetailDialog *dlg = new DownloadDetailDialog(id, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    }
}

void MainWindow::onTableContextMenu(const QPoint &pos) {
    int row = m_downloadTable->rowAt(pos.y());
    if (row < 0) return;

    QTableWidgetItem *item0 = m_downloadTable->item(row, 0);
    if (item0 == NULL) return;
    QString id = item0->data(Qt::UserRole).toString();
    DownloadItem *item = DownloadManager::instance().findItem(id);
    if (item == NULL) return;

    const QString filePath = item->targetFilePath();
    const QString dirPath = item->m_directory;
    const QString urlText = item->m_url.toString();
    const bool isActive = item->isActive();
    const DownloadState state = item->m_state;

    QMenu menu(this);
    if (isActive) {
        QAction *actPause = menu.addAction(tr("Duraklat"));
        connect(actPause, &QAction::triggered, [id]() { DownloadManager::instance().pauseDownload(id); });
    } else if (state == DownloadStatePaused) {
        QAction *actResume = menu.addAction(tr("Devam Et"));
        connect(actResume, &QAction::triggered, [id]() { DownloadManager::instance().resumeDownload(id); });
    }

    QAction *actDetail = menu.addAction(tr("İndirme Detaylarını Göster"));
    connect(actDetail, &QAction::triggered, [this, id]() {
        DownloadDetailDialog *dlg = new DownloadDetailDialog(id, this);
        dlg->setAttribute(Qt::WA_DeleteOnClose);
        dlg->show();
    });

    menu.addSeparator();

    if (state == DownloadStateCompleted) {
        QAction *actOpen = menu.addAction(tr("Dosyayı Aç"));
        connect(actOpen, &QAction::triggered, [filePath]() {
            QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
        });
    }

    QAction *actShowFolder = menu.addAction(tr("Klasörde Göster"));
    connect(actShowFolder, &QAction::triggered, [dirPath]() {
        QDesktopServices::openUrl(QUrl::fromLocalFile(dirPath));
    });

    QAction *actCopyUrl = menu.addAction(tr("Bağlantıyı Kopyala"));
    connect(actCopyUrl, &QAction::triggered, [urlText]() {
        QApplication::clipboard()->setText(urlText);
    });

    menu.addSeparator();
    QAction *actRemove = menu.addAction(tr("Listeden Kaldır"));
    connect(actRemove, &QAction::triggered, [id]() { DownloadManager::instance().removeDownload(id, false); });

    QAction *actDelete = menu.addAction(tr("Dosyayı ve Listeden Sil"));
    connect(actDelete, &QAction::triggered, [this, id]() {
        QMessageBox::StandardButton reply = QMessageBox::warning(
            this, tr("Dosyayı Sil"),
            tr("Bu indirmeyi listeden kaldırmak ve indirilen dosyayı sabit diskten tamamen silmek istediğinizden emin misiniz?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel
        );
        if (reply == QMessageBox::Yes) {
            DownloadManager::instance().removeDownload(id, true);
        }
    });

    menu.exec(m_downloadTable->viewport()->mapToGlobal(pos));
}

void MainWindow::onItemAdded(const DownloadItem &item) {
    Q_UNUSED(item);
    refreshTable();
}

void MainWindow::onItemUpdated(const DownloadItem &item) {
    int row = findRowById(item.m_id);
    if (row >= 0) {
        updateTableRow(row, item);
    }

    // Status bar güncelle
    int active = DownloadManager::instance().activeCount();
    m_statusActiveLabel->setText(tr("%1 Aktif İndirme").arg(active));

    double totalSpeed = 0.0;
    const QList<DownloadItem> &list = DownloadManager::instance().items();
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).isActive()) {
            totalSpeed += list.at(i).m_speed;
        }
    }

    QString speedStr;
    if (totalSpeed < 1024) speedStr = QString::number(totalSpeed, 'f', 0) + " B/s";
    else if (totalSpeed < 1024 * 1024) speedStr = QString::number(totalSpeed / 1024.0, 'f', 1) + " KB/s";
    else speedStr = QString::number(totalSpeed / (1024.0 * 1024.0), 'f', 2) + " MB/s";

    m_statusSpeedLabel->setText(tr("Toplam Hız: %1").arg(speedStr));
}

void MainWindow::onItemRemoved(const QString &id) {
    int row = findRowById(id);
    if (row >= 0) {
        m_downloadTable->removeRow(row);
    }
}

void MainWindow::onUrlDetected(const QUrl &url) {
    NewDownloadDialog dlg(url, this);
    if (dlg.exec() == QDialog::Accepted) {
        DownloadManager::instance().addDownload(dlg.downloadItem(), dlg.shouldStartImmediately());
    }
}

void MainWindow::onQuitTriggered() {
    if (DownloadManager::instance().activeCount() > 0) {
        int ret = QMessageBox::question(
            this, tr("Çıkış Onayı"),
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

void MainWindow::closeEvent(QCloseEvent *event) {
    if (qApp->isSavingSession()) {
        event->accept();
        return;
    }

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        // Sistem tepsisi varsa pencereyi gizle, arka planda çalışmaya devam et
        hide();
        event->ignore();
    } else {
        // Sistem tepsisi yoksa kapatma uygulamadan çıkış anlamına gelir
        if (DownloadManager::instance().activeCount() > 0) {
            int ret = QMessageBox::question(
                this, tr("Çıkış Onayı"),
                tr("Aktif indirmeler devam ediyor. Uygulamadan çıkmak istediğinizden emin misiniz?"),
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );
            if (ret != QMessageBox::Yes) {
                event->ignore();
                return;
            }
        }
        DownloadManager::instance().pauseAll();
        event->accept();
        QApplication::quit();
    }
}
