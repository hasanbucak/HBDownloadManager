#include "SettingsDialog.h"
#include "../core/DownloadManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QGroupBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QMessageBox>

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent),
      m_directoryEdit(new QLineEdit(this)),
      m_browseBtn(new QPushButton(tr("Gözat..."), this)),
      m_segmentsSpin(new QSpinBox(this)),
      m_concurrentSpin(new QSpinBox(this)),
      m_portSpin(new QSpinBox(this)),
      m_clipboardCheck(new QCheckBox(tr("Panodaki bağlantıları izle: arşiv, program, video, ses, pdf/belge"), this)),
      m_installBtn(new QPushButton(tr("Tarayıcı Eklenti Köprüsünü Kur (1-Tık Kurulum)"), this)),
      m_okBtn(new QPushButton(tr("Kaydet"), this)),
      m_cancelBtn(new QPushButton(tr("İptal"), this)),
      m_initialPort(0) {
    setupUi();

    DownloadManager &dm = DownloadManager::instance();
    m_directoryEdit->setText(dm.defaultDirectory());
    m_segmentsSpin->setValue(dm.defaultSegments());
    m_concurrentSpin->setValue(dm.maxConcurrentDownloads());
    m_initialPort = dm.serverPort();
    m_portSpin->setValue(m_initialPort);
    m_clipboardCheck->setChecked(dm.clipboardMonitoringEnabled());

    connect(m_browseBtn, SIGNAL(clicked()), this, SLOT(onBrowseClicked()));
    connect(m_okBtn, SIGNAL(clicked()), this, SLOT(onOkClicked()));
    connect(m_cancelBtn, SIGNAL(clicked()), this, SLOT(reject()));
    connect(m_installBtn, SIGNAL(clicked()), this, SIGNAL(atInstallBridgeRequested()));
}

SettingsDialog::~SettingsDialog() {
}

void SettingsDialog::setupUi() {
    setWindowTitle(tr("Ayarlar ve Tarayıcı Entegrasyonu"));
    resize(560, 480);

    m_segmentsSpin->setRange(1, 16);
    m_concurrentSpin->setRange(1, 10);
    m_portSpin->setRange(1024, 65535);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QGroupBox *generalBox = new QGroupBox(tr("Genel"), this);
    QFormLayout *form = new QFormLayout(generalBox);

    QHBoxLayout *dirLayout = new QHBoxLayout();
    dirLayout->addWidget(m_directoryEdit);
    dirLayout->addWidget(m_browseBtn);
    form->addRow(tr("Varsayılan Kayıt Yeri:"), dirLayout);
    form->addRow(tr("Varsayılan Bağlantı Sayısı (1-16):"), m_segmentsSpin);
    form->addRow(tr("Eşzamanlı İndirme Sayısı:"), m_concurrentSpin);
    form->addRow(tr("Köprü Portu:"), m_portSpin);

    QLabel *portNote = new QLabel(tr("Port değişikliği uygulama yeniden başlatıldıktan sonra geçerli olur. "
                                     "Tarayıcı eklentisi varsayılan olarak 48213 portunu kullanır."), this);
    portNote->setWordWrap(true);
    // The wrapped label was clipped: reserve room for three lines regardless of the wrap width
    portNote->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::MinimumExpanding);
    portNote->setMinimumHeight(portNote->fontMetrics().lineSpacing() * 3 + 4);
    form->addRow(portNote);

    form->addRow(m_clipboardCheck);
    mainLayout->addWidget(generalBox);

    QGroupBox *bridgeBox = new QGroupBox(tr("Tarayıcı Entegrasyonu"), this);
    QVBoxLayout *bridgeLayout = new QVBoxLayout(bridgeBox);
    QLabel *infoLabel = new QLabel(tr("<b>Chrome, Opera, Brave, Edge, Firefox, Chromium</b><br>"
                                      "Tarayıcılardan indirmelerin otomatik yakalanması için sistem native host kayıtlarını oluşturun:"), this);
    infoLabel->setWordWrap(true);
    bridgeLayout->addWidget(infoLabel);
    bridgeLayout->addWidget(m_installBtn);
    mainLayout->addWidget(bridgeBox);

    mainLayout->addStretch();

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_okBtn);
    m_okBtn->setDefault(true);
    mainLayout->addLayout(btnLayout);
}

void SettingsDialog::onBrowseClicked() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("İndirme Dizinini Seç"), m_directoryEdit->text());
    if (!dir.isEmpty()) {
        m_directoryEdit->setText(dir);
    }
}

bool SettingsDialog::validateDirectory(QString *dirOut) {
    const QString dir = m_directoryEdit->text().trimmed();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, tr("Geçersiz Klasör"), tr("Lütfen bir kayıt klasörü girin."));
        return false;
    }
    if (!QDir().mkpath(dir)) {
        QMessageBox::warning(this, tr("Geçersiz Klasör"), tr("Klasör oluşturulamadı: %1").arg(dir));
        return false;
    }
    if (!QFileInfo(dir).isWritable()) {
        QMessageBox::warning(this, tr("Geçersiz Klasör"), tr("Bu klasöre yazma izniniz yok: %1").arg(dir));
        return false;
    }
    *dirOut = QDir::cleanPath(QFileInfo(dir).absoluteFilePath());
    return true;
}

void SettingsDialog::onOkClicked() {
    QString dir;
    if (!validateDirectory(&dir)) {
        return;
    }

    DownloadManager &dm = DownloadManager::instance();
    dm.setDefaultDirectory(dir);
    dm.setDefaultSegments(m_segmentsSpin->value());
    dm.setMaxConcurrentDownloads(m_concurrentSpin->value());
    dm.setClipboardMonitoringEnabled(m_clipboardCheck->isChecked());

    const quint16 newPort = static_cast<quint16>(m_portSpin->value());
    if (newPort != m_initialPort) {
        dm.setServerPort(newPort);
        QMessageBox::information(this, tr("Yeniden Başlatma Gerekli"),
                                 tr("Köprü portu %1 olarak kaydedildi. Değişiklik uygulama yeniden başlatıldığında geçerli olur.\n"
                                    "Tarayıcı eklentisinin de aynı portu kullanacak şekilde ayarlanması gerekebilir.").arg(newPort));
    }
    accept();
}
