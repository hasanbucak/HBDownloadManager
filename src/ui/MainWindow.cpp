#include <QCoreApplication>
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
#include <QSettings>
#include <QTimer>
#include <QShortcut>
#include <QDir>
#include <QFile>
#include "SettingsDialog.h"
#include "../core/VideoDownloader.h"

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
      m_statusToolsLabel(new QLabel(this)),
      m_selectedCategory(0),
      m_dialogOpen(false) {
    setupUi();
    applyDarkTheme();
    refreshTable();
    restoreWindowState();
    updateToolWarnings();
    updateStatusBar();
    updateActionStates();

    connect(&DownloadManager::instance(), SIGNAL(atItemAdded(DownloadItem)),
            this, SLOT(onItemAdded(DownloadItem)));
    connect(&DownloadManager::instance(), SIGNAL(atItemUpdated(DownloadItem)),
            this, SLOT(onItemUpdated(DownloadItem)));
    connect(&DownloadManager::instance(), SIGNAL(atItemRemoved(QString)),
            this, SLOT(onItemRemoved(QString)));
}

MainWindow::~MainWindow() {
    saveWindowState();
}

void MainWindow::saveWindowState() {
    QSettings settings("HasanBucak", "hb-download-manager");
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/state", QMainWindow::saveState());
}

void MainWindow::restoreWindowState() {
    QSettings settings("HasanBucak", "hb-download-manager");
    const QByteArray geo = settings.value("window/geometry").toByteArray();
    if (!geo.isEmpty()) {
        restoreGeometry(geo);
    }
    const QByteArray st = settings.value("window/state").toByteArray();
    if (!st.isEmpty()) {
        QMainWindow::restoreState(st);
    }
}

void MainWindow::updateToolWarnings() {
    // yt-dlp downloads video; it merges video and audio with ffmpeg. Tell the user when either is missing.
    const bool noYtDlp = !VideoDownloader::isAvailable();
    const bool noFfmpeg = !VideoDownloader::isFfmpegAvailable();
    QStringList tips;
    if (noYtDlp) {
        tips << tr("yt-dlp bulunamadı: video ve medya indirmeleri çalışmaz. yt-dlp paketini kurun.");
    }
    if (noFfmpeg) {
        tips << tr("ffmpeg bulunamadı: video/ses birleştirme gerektiren medya indirmeleri başarısız olabilir. ffmpeg paketini kurun.");
    }

    if (noYtDlp && noFfmpeg) {
        m_statusToolsLabel->setText(tr("Uyarı: yt-dlp ve ffmpeg yok"));
    } else if (noYtDlp) {
        m_statusToolsLabel->setText(tr("Uyarı: yt-dlp yok"));
    } else if (noFfmpeg) {
        m_statusToolsLabel->setText(tr("Uyarı: ffmpeg yok"));
    }

    if (noYtDlp || noFfmpeg) {
        m_statusToolsLabel->setToolTip(tips.join("\n"));
        m_statusToolsLabel->setStyleSheet("color: #ffd166; font-weight: bold;");
        m_statusToolsLabel->show();
    } else {
        m_statusToolsLabel->clear();
        m_statusToolsLabel->hide();
    }
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
    resize(1100, 640);

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
    // Explicit QActions: the (text, receiver, member, shortcut) overload is deprecated in Qt 6
    QAction *actNewMenu = fileMenu->addAction(tr("&Yeni İndirme..."));
    actNewMenu->setShortcut(QKeySequence::New);
    connect(actNewMenu, SIGNAL(triggered()), this, SLOT(onNewDownloadClicked()));
    fileMenu->addSeparator();
    m_actQuit = fileMenu->addAction(tr("&Çıkış"));
    m_actQuit->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q));
    connect(m_actQuit, SIGNAL(triggered()), this, SLOT(onQuitTriggered()));

    QMenu *helpMenu = menuBar()->addMenu(tr("&Yardım"));
    QAction *actAbout = helpMenu->addAction(tr("&Hakkında"));
    connect(actAbout, SIGNAL(triggered()), this, SLOT(onAboutTriggered()));
}

void MainWindow::onAboutTriggered() {
    QMessageBox::about(this, tr("Hakkında"),
                       tr("<b>HB Download Manager</b><br>Sürüm %1<br><br>"
                          "Çok parçalı indirme, yt-dlp ile video indirme ve tarayıcı eklentisi desteği.")
                           .arg(QCoreApplication::applicationVersion().toHtmlEscaped()));
}

void MainWindow::setupToolBar() {
    m_toolBar = addToolBar(tr("Ana Araç Çubuğu"));
    m_toolBar->setObjectName("mainToolBar");
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

    // The file name takes whatever the other, compact columns leave; together they fit the default
    // window, so the name never collapses to the minimum section width
    m_downloadTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Interactive);
    m_downloadTable->setColumnWidth(2, 110);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    // Status can hold a long error text: fixed width (elided, full text in the tooltip)
    m_downloadTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive);
    m_downloadTable->setColumnWidth(5, 130);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    m_downloadTable->setWordWrap(false);  // with wrapping on, Qt elides at the end regardless of the mode
    m_downloadTable->setTextElideMode(Qt::ElideMiddle);  // keeps the extension of a long name visible

    m_downloadTable->horizontalHeader()->setMinimumSectionSize(60);
    // The white row-number strip is useless (and ugly in the dark theme)
    m_downloadTable->verticalHeader()->setVisible(false);

    m_downloadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_downloadTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_downloadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_downloadTable->setContextMenuPolicy(Qt::CustomContextMenu);

    // Delete removes, Enter opens; the view ignores both so they would only reach the splitter
    const int keys[] = { Qt::Key_Delete, Qt::Key_Return, Qt::Key_Enter };
    for (int i = 0; i < 3; ++i) {
        QShortcut *sc = new QShortcut(QKeySequence(keys[i]), m_downloadTable);
        sc->setContext(Qt::WidgetShortcut);
        if (keys[i] == Qt::Key_Delete) {
            connect(sc, SIGNAL(activated()), this, SLOT(onRemoveClicked()));
        } else {
            connect(sc, SIGNAL(activated()), this, SLOT(onEnterPressed()));
        }
    }
    connect(m_downloadTable, SIGNAL(itemSelectionChanged()), this, SLOT(updateActionStates()));
    connect(m_downloadTable, SIGNAL(currentCellChanged(int,int,int,int)), this, SLOT(updateActionStates()));

    connect(m_downloadTable, SIGNAL(cellDoubleClicked(int,int)), this, SLOT(onTableDoubleClicked(int,int)));
    connect(m_downloadTable, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(onTableContextMenu(QPoint)));
}

void MainWindow::setupStatusBar() {
    statusBar()->addWidget(m_statusActiveLabel);
    statusBar()->addPermanentWidget(m_statusToolsLabel);
    statusBar()->addPermanentWidget(m_statusSpeedLabel);
    statusBar()->addPermanentWidget(m_statusServerLabel);

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
        "QGroupBox { color: #ffffff; border: 1px solid #3c3c3c; border-radius: 4px; margin-top: 10px; padding-top: 10px; font-weight: bold; }"
        "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 0 4px; color: #cccccc; }"
        "QRadioButton { color: #ffffff; spacing: 6px; }"
        "QCheckBox { color: #ffffff; spacing: 6px; }"
        "QSpinBox { background-color: #2d2d2d; color: #ffffff; border: 1px solid #444444; border-radius: 4px; padding: 4px; }"
    );
}

bool MainWindow::matchesFilter(const DownloadItem &it) const {
    switch (m_selectedCategory) {
    case 0: return true; // Tümü
    case 1: return it.isActive(); // Devam Edenler
    case 2: return (it.m_state == DownloadStatePaused); // Duraklatılanlar
    case 3: return (it.m_state == DownloadStateCompleted); // Tamamlananlar
    case 4: return (it.category() == FileCategoryImage);
    case 5: return (it.category() == FileCategoryMusic);
    case 6: return (it.category() == FileCategoryVideo);
    case 7: return (it.category() == FileCategoryPrograms);
    case 8: return (it.category() == FileCategoryDocument);
    case 9: return (it.category() == FileCategoryCompressed);
    default: return true;
    }
}

void MainWindow::onCategorySelected(int row) {
    m_selectedCategory = row;
    refreshTable();
}

void MainWindow::refreshTable() {
    QString selId = selectedDownloadId();
    m_downloadTable->setRowCount(0);
    const QList<DownloadItem> &items = DownloadManager::instance().items();

    for (int i = 0; i < items.size(); ++i) {
        const DownloadItem &it = items.at(i);
        if (matchesFilter(it)) {
            int row = m_downloadTable->rowCount();
            m_downloadTable->insertRow(row);
            updateTableRow(row, it);
        }
    }

    if (!selId.isEmpty()) {
        int selRow = findRowById(selId);
        if (selRow >= 0) {
            m_downloadTable->selectRow(selRow);
        }
    }
    updateActionStates();
}

void MainWindow::setCellText(int row, int column, const QString &text, const QString &tooltip) {
    // Update in place: replacing items every tick resets hover/selection state and churns the allocator
    QTableWidgetItem *cell = m_downloadTable->item(row, column);
    if (cell == NULL) {
        cell = new QTableWidgetItem(text);
        m_downloadTable->setItem(row, column, cell);
    } else if (cell->text() != text) {
        cell->setText(text);
    }
    if (cell->toolTip() != tooltip) {
        cell->setToolTip(tooltip);
    }
}

void MainWindow::updateTableRow(int row, const DownloadItem &item) {
    QTableWidgetItem *item0 = m_downloadTable->item(row, 0);
    if (item0 == NULL) {
        item0 = new QTableWidgetItem(item.m_fileName);
        m_downloadTable->setItem(row, 0, item0);
    } else if (item0->text() != item.m_fileName) {
        item0->setText(item.m_fileName);
    }
    item0->setData(Qt::UserRole, item.m_id);
    item0->setToolTip(item.m_fileName);

    setCellText(row, 1, item.formattedSize());

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
    // Unknown size while running: a busy bar instead of a frozen 0%
    const bool sizeUnknown = (item.m_totalBytes <= 0 && item.progress() <= 0.0);
    if (item.isActive() && sizeUnknown) {
        if (pb->maximum() != 0) {
            pb->setRange(0, 0);
        }
    } else {
        if (pb->maximum() == 0) {
            pb->setRange(0, 100);
        }
        pb->setValue(static_cast<int>(item.progress() * 100.0));
    }

    setCellText(row, 3, item.formattedSpeed());
    setCellText(row, 4, item.formattedEta());

    QString statusText = item.stateString();
    QString statusTip;
    if (item.m_state == DownloadStateError && !item.m_errorMessage.isEmpty()) {
        QString shortMsg = item.m_errorMessage.section('\n', 0, 0).trimmed();
        if (shortMsg.size() > 40) {
            shortMsg = shortMsg.left(37) + "...";
        }
        statusText = tr("Hata: %1").arg(shortMsg);
        statusTip = item.m_errorMessage;
    } else if (item.isActive() && !item.m_stage.isEmpty() &&
               (item.m_kind == DownloadKindVideo || item.m_kind == DownloadKindAudio)) {
        statusText = item.m_stage;
    }
    setCellText(row, 5, statusText, statusTip);

    QString dateStr = item.m_createdAt.isValid()
        ? item.m_createdAt.toString("dd.MM.yy hh:mm")
        : QDateTime::currentDateTime().toString("dd.MM.yy hh:mm");
    setCellText(row, 6, dateStr);
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
    requestNewDownload();
}

void MainWindow::requestNewDownload() {
    onUrlDetected(QUrl());
}

static bool canPauseItem(const DownloadItem &it) {
    return it.m_state == DownloadStateQueued || it.isActive();
}

static bool canResumeItem(const DownloadItem &it) {
    return it.m_state == DownloadStatePaused || it.m_state == DownloadStateError ||
           it.m_state == DownloadStateCancelled;
}

static QString resumeLabel(const DownloadItem &it) {
    return it.m_state == DownloadStateError ? QObject::tr("Tekrar Dene") : QObject::tr("Devam Et");
}

void MainWindow::updateActionStates() {
    const QString id = selectedDownloadId();
    const DownloadItem *item = id.isEmpty() ? NULL : DownloadManager::instance().findItem(id);
    m_actPause->setEnabled(item != NULL && canPauseItem(*item));
    m_actResume->setEnabled(item != NULL && canResumeItem(*item));
    m_actRemove->setEnabled(item != NULL);
    if (item != NULL && item->m_state == DownloadStateError) {
        m_actResume->setText(tr("Tekrar Dene [▶]"));
    } else {
        m_actResume->setText(tr("Devam Et [▶]"));
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

void MainWindow::submitDownload(const DownloadItem &item, bool startImmediately) {
    const int before = DownloadManager::instance().items().size();
    const QString id = DownloadManager::instance().addDownload(item, startImmediately);
    if (id.isEmpty()) return;
    if (DownloadManager::instance().items().size() == before) {
        // addDownload returned the id of the copy that is already running
        selectRowById(id);
        statusBar()->showMessage(tr("Bu bağlantı zaten indiriliyor"), 6000);
    }
}

void MainWindow::selectRowById(const QString &id) {
    int row = findRowById(id);
    if (row < 0) {
        // The row may be hidden by the current category filter: show everything
        m_categoryList->setCurrentRow(0);
        row = findRowById(id);
    }
    if (row >= 0) {
        m_downloadTable->selectRow(row);
        m_downloadTable->scrollToItem(m_downloadTable->item(row, 0));
    }
}

void MainWindow::showDetails(const QString &id) {
    DownloadDetailDialog *dlg = new DownloadDetailDialog(id, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->show();
}

void MainWindow::openDownloadedFile(const QString &id) {
    DownloadItem *item = DownloadManager::instance().findItem(id);
    if (item == NULL) return;
    const QString path = item->targetFilePath();
    if (!QFileInfo::exists(path)) {
        QMessageBox::warning(this, tr("Dosya Bulunamadı"),
                             tr("Dosya artık bu konumda değil (taşınmış veya silinmiş olabilir):\n%1").arg(path));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void MainWindow::showDownloadFolder(const QString &id) {
    DownloadItem *item = DownloadManager::instance().findItem(id);
    if (item == NULL) return;
    const QString dir = item->m_directory;
    if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
        QMessageBox::warning(this, tr("Klasör Bulunamadı"),
                             tr("Klasör artık mevcut değil:\n%1").arg(dir));
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void MainWindow::onEnterPressed() {
    onTableDoubleClicked(m_downloadTable->currentRow(), 0);
}

void MainWindow::onRemoveClicked() {
    removeWithConfirmation(selectedDownloadId());
}

void MainWindow::removeWithConfirmation(const QString &id) {
    if (id.isEmpty()) return;

    DownloadItem *item = DownloadManager::instance().findItem(id);
    if (!item) return;

    if (item->m_state == DownloadStateCompleted) {
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

        // exec() ran an event loop: the item may have changed or vanished meanwhile
        if (DownloadManager::instance().findItem(id) == NULL) return;
        if (msgBox.clickedButton() == btnDeleteFile) {
            DownloadManager::instance().removeDownload(id, true);
        } else if (msgBox.clickedButton() == btnRemoveOnly) {
            DownloadManager::instance().removeDownload(id, false);
        }
    } else if (item->m_state == DownloadStateCancelled) {
        DownloadManager::instance().removeDownload(id, false);
    } else {
        // D1: Explicit confirmation naming the lost partial data (plain wording when nothing was downloaded)
        QMessageBox::StandardButton reply;
        if (item->m_receivedBytes <= 0) {
            reply = QMessageBox::question(
                this, tr("Listeden Kaldır"), tr("Listeden kaldırılsın mı?"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        } else {
            double mb = item->m_receivedBytes / (1024.0 * 1024.0);
            QString mbStr = QString::number(mb, 'f', 1);
            reply = QMessageBox::warning(
                this, tr("Listeden Kaldır"),
                tr("Kısmen indirilen veri (%1 MB) silinecek. İndirmeyi listeden kaldırmak istediğinizden emin misiniz?").arg(mbStr),
                QMessageBox::Yes | QMessageBox::Cancel,
                QMessageBox::Cancel);
        }
        if (reply != QMessageBox::Yes) return;

        // U4: the download may have completed while the box was open: never delete a finished file
        DownloadItem *again = DownloadManager::instance().findItem(id);
        if (again == NULL) return;
        if (again->m_state == DownloadStateCompleted) {
            statusBar()->showMessage(tr("İndirme onay penceresi açıkken tamamlandı; dosya silinmedi, yalnızca listeden kaldırıldı."), 8000);
            DownloadManager::instance().removeDownload(id, false);
        } else {
            DownloadManager::instance().removeDownload(id, true);
        }
    }
}

void MainWindow::onClearCompletedClicked() {
    const QList<DownloadItem> &list = DownloadManager::instance().items();
    QStringList toRemove;
    for (int i = 0; i < list.size(); ++i) {
        if (list.at(i).m_state == DownloadStateCompleted) {
            toRemove.append(list.at(i).m_id);
        }
    }

    for (int i = 0; i < toRemove.size(); ++i) {
        DownloadManager::instance().removeDownload(toRemove.at(i), false);
    }
}

void MainWindow::onSettingsClicked() {
    SettingsDialog dlg(this);
    connect(&dlg, SIGNAL(atInstallBridgeRequested()), this, SLOT(onInstallHostBridgeClicked()));
    dlg.exec();
}

void MainWindow::onInstallHostBridgeClicked() {
    QString hostExe = QApplication::applicationDirPath() + "/hbdm-native-host";
    if (QFile::exists(hostExe)) {
        const int rc = QProcess::execute(hostExe, QStringList() << "--install");
        if (rc == 0) {
            QMessageBox::information(this, tr("Başarılı"), tr("Tarayıcı native host kayıtları başarıyla yüklendi!\nChrome, Opera, Firefox, Brave ve Edge eklentileri artık hazır."));
        } else if (rc == -2) {
            QMessageBox::warning(this, tr("Uyarı"), tr("Native host çalıştırılamadı: %1").arg(hostExe));
        } else if (rc == -1) {
            QMessageBox::warning(this, tr("Uyarı"), tr("Native host kurulum sırasında çöktü."));
        } else {
            QMessageBox::warning(this, tr("Uyarı"), tr("Native host kurulumu başarısız oldu (çıkış kodu %1).").arg(rc));
        }
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
        openDownloadedFile(id);
    } else {
        // İndiriliyorsa veya duraklatılmışsa detay penceresini aç
        showDetails(id);
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

    const QString urlText = item->m_url.toString();
    const DownloadState state = item->m_state;

    QMenu menu(this);
    if (canPauseItem(*item)) {
        QAction *actPause = menu.addAction(tr("Duraklat"));
        connect(actPause, &QAction::triggered, [id]() { DownloadManager::instance().pauseDownload(id); });
    } else if (canResumeItem(*item)) {
        QAction *actResume = menu.addAction(resumeLabel(*item));
        connect(actResume, &QAction::triggered, [id]() { DownloadManager::instance().resumeDownload(id); });
    }

    QAction *actDetail = menu.addAction(tr("İndirme Detaylarını Göster"));
    connect(actDetail, &QAction::triggered, [this, id]() { showDetails(id); });

    menu.addSeparator();

    if (state == DownloadStateCompleted) {
        QAction *actOpen = menu.addAction(tr("Dosyayı Aç"));
        connect(actOpen, &QAction::triggered, [this, id]() { openDownloadedFile(id); });
    }

    QAction *actShowFolder = menu.addAction(tr("Klasörde Göster"));
    connect(actShowFolder, &QAction::triggered, [this, id]() { showDownloadFolder(id); });

    QAction *actCopyUrl = menu.addAction(tr("Bağlantıyı Kopyala"));
    connect(actCopyUrl, &QAction::triggered, [urlText]() {
        ClipboardWatcher::ignoreUrl(urlText);
        QApplication::clipboard()->setText(urlText);
    });

    menu.addSeparator();
    QAction *actRemove = menu.addAction(tr("Listeden Kaldır"));
    connect(actRemove, &QAction::triggered, [this, id]() {
        removeWithConfirmation(id);
    });

    QAction *actDelete = menu.addAction(tr("Dosyayı ve Listeden Sil"));
    connect(actDelete, &QAction::triggered, [this, id]() {
        QMessageBox::StandardButton reply = QMessageBox::warning(
            this, tr("Dosyayı Sil"),
            tr("Bu indirmeyi listeden kaldırmak ve indirilen dosyayı sabit diskten tamamen silmek istediğinizden emin misiniz?"),
            QMessageBox::Yes | QMessageBox::Cancel,
            QMessageBox::Cancel
        );
        if (reply == QMessageBox::Yes && DownloadManager::instance().findItem(id) != NULL) {
            DownloadManager::instance().removeDownload(id, true);
        }
    });

    menu.exec(m_downloadTable->viewport()->mapToGlobal(pos));
}

void MainWindow::onItemAdded(const DownloadItem &item) {
    if (matchesFilter(item)) {
        // Newest first, the same order the list has after a restart
        m_downloadTable->insertRow(0);
        updateTableRow(0, item);
        m_downloadTable->scrollToTop();
    }
    updateStatusBar();
    updateActionStates();
}

void MainWindow::onItemUpdated(const DownloadItem &item) {
    int row = findRowById(item.m_id);
    bool match = matchesFilter(item);

    if (row >= 0) {
        if (!match) {
            m_downloadTable->removeRow(row);
        } else {
            updateTableRow(row, item);
        }
    } else {
        if (match) {
            m_downloadTable->insertRow(0);
            updateTableRow(0, item);
        }
    }

    updateStatusBar();
    if (item.m_id == selectedDownloadId()) {
        updateActionStates();
    }
}

void MainWindow::updateStatusBar() {
    int active = DownloadManager::instance().activeCount();
    m_statusActiveLabel->setText(tr("%n etkin indirme", "", active));

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
    updateStatusBar();
    updateActionStates();
}

void MainWindow::onUrlDetected(const QUrl &url) {
    // Never open a dialog from inside the caller's stack (socket lambda, clipboard signal):
    // queue the URL and let processUrlQueue() run from the event loop.
    // An empty QUrl stands for a blank "new download" dialog; identical entries are not queued twice.
    if (!m_pendingUrls.contains(url)) {
        m_pendingUrls.enqueue(url);
    }
    if (!m_dialogOpen) {
        QTimer::singleShot(0, this, SLOT(processUrlQueue()));
    }
}

void MainWindow::processUrlQueue() {
    if (m_dialogOpen) return;
    m_dialogOpen = true;
    // Stop opening dialogs once the application is quitting (exit() only ends the loops running now)
    while (!m_pendingUrls.isEmpty() && !QCoreApplication::instance()->property("hbdmQuitting").toBool()) {
        const QUrl url = m_pendingUrls.dequeue();
        NewDownloadDialog dlg(url, this);
        if (dlg.exec() == QDialog::Accepted) {
            submitDownload(dlg.downloadItem(), dlg.shouldStartImmediately());
        }
        // U3: a cancelled dialog must not block the same clipboard URL forever
        ClipboardWatcher::resetLastUrl();
    }
    m_dialogOpen = false;
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
    QCoreApplication::instance()->setProperty("hbdmQuitting", true);
    QCoreApplication::exit(0);  // quit() may be refused by closeEvent in Qt 6
}

void MainWindow::closeEvent(QCloseEvent *event) {
    saveWindowState();
    if (qApp->isSavingSession()) {
        DownloadManager::instance().pauseAll();
        event->accept();
        QCoreApplication::instance()->setProperty("hbdmQuitting", true);
        QCoreApplication::exit(0);
        return;
    }

    if (QSystemTrayIcon::isSystemTrayAvailable()) {
        // Sistem tepsisi varsa pencereyi gizle, arka planda çalışmaya devam et
        hide();
        event->ignore();
        emit atHiddenToTray();
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
        QCoreApplication::instance()->setProperty("hbdmQuitting", true);
        QCoreApplication::exit(0);  // quit() may be refused by closeEvent in Qt 6
    }
}
