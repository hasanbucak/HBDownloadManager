#include "NewDownloadDialog.h"
#include "../core/DownloadManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QGroupBox>

NewDownloadDialog::NewDownloadDialog(const QUrl &url, QWidget *parent)
    : QDialog(parent),
      m_urlEdit(new QLineEdit(this)),
      m_fileNameEdit(new QLineEdit(this)),
      m_directoryEdit(new QLineEdit(this)),
      m_segmentsCombo(new QComboBox(this)),
      m_fileRadio(new QRadioButton(tr("Normal Dosya"), this)),
      m_videoRadio(new QRadioButton(tr("Video / Medya Akışı (yt-dlp)"), this)),
      m_browseBtn(new QPushButton(tr("Gözat..."), this)),
      m_startBtn(new QPushButton(tr("Hemen İndir"), this)),
      m_laterBtn(new QPushButton(tr("Daha Sonra İndir"), this)),
      m_cancelBtn(new QPushButton(tr("İptal"), this)),
      m_startImmediately(true) {
    setupUi();

    if (url.isValid()) {
        m_urlEdit->setText(url.toString());
        QString name = QFileInfo(url.path()).fileName();
        if (name.isEmpty()) name = "download";
        m_fileNameEdit->setText(name);

        QString host = url.host().toLower();
        if (host.contains("youtube.com") || host.contains("youtu.be") ||
            host.contains("vimeo.com") || host.contains("tiktok.com")) {
            m_videoRadio->setChecked(true);
        } else {
            m_fileRadio->setChecked(true);
        }
    } else {
        m_fileRadio->setChecked(true);
    }

    m_directoryEdit->setText(DownloadManager::instance().defaultDirectory());

    connect(m_browseBtn, SIGNAL(clicked()), this, SLOT(onBrowseClicked()));
    connect(m_startBtn, SIGNAL(clicked()), this, SLOT(onStartClicked()));
    connect(m_laterBtn, SIGNAL(clicked()), this, SLOT(onLaterClicked()));
    connect(m_cancelBtn, SIGNAL(clicked()), this, SLOT(reject()));
}

NewDownloadDialog::~NewDownloadDialog() {
}

void NewDownloadDialog::setupUi() {
    setWindowTitle(tr("Yeni İndirme Başlat"));
    resize(520, 260);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    QFormLayout *formLayout = new QFormLayout();

    formLayout->addRow(tr("Bağlantı (URL):"), m_urlEdit);
    formLayout->addRow(tr("Dosya Adı:"), m_fileNameEdit);

    QHBoxLayout *dirLayout = new QHBoxLayout();
    dirLayout->addWidget(m_directoryEdit);
    dirLayout->addWidget(m_browseBtn);
    formLayout->addRow(tr("Kayıt Yeri:"), dirLayout);

    m_segmentsCombo->addItem("1", 1);
    m_segmentsCombo->addItem("2", 2);
    m_segmentsCombo->addItem("4", 4);
    m_segmentsCombo->addItem("8 (Önerilen)", 8);
    m_segmentsCombo->addItem("16 (Maksimum)", 16);
    m_segmentsCombo->setCurrentIndex(3);
    formLayout->addRow(tr("Bağlantı Sayısı:"), m_segmentsCombo);

    QHBoxLayout *kindLayout = new QHBoxLayout();
    kindLayout->addWidget(m_fileRadio);
    kindLayout->addWidget(m_videoRadio);
    kindLayout->addStretch();
    formLayout->addRow(tr("İndirme Türü:"), kindLayout);

    mainLayout->addLayout(formLayout);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    btnLayout->addWidget(m_cancelBtn);
    btnLayout->addWidget(m_laterBtn);
    btnLayout->addWidget(m_startBtn);
    m_startBtn->setDefault(true);

    mainLayout->addLayout(btnLayout);
}

void NewDownloadDialog::onBrowseClicked() {
    QString dir = QFileDialog::getExistingDirectory(this, tr("İndirme Dizinini Seç"), m_directoryEdit->text());
    if (!dir.isEmpty()) {
        m_directoryEdit->setText(dir);
    }
}

DownloadItem NewDownloadDialog::downloadItem() const {
    QUrl url(m_urlEdit->text().trimmed());
    DownloadItem item(url, m_directoryEdit->text().trimmed());

    QString name = m_fileNameEdit->text().trimmed();
    if (!name.isEmpty()) {
        item.m_fileName = name;
    }
    item.m_kind = (m_videoRadio->isChecked() ? DownloadKindVideo : DownloadKindFile);
    item.m_requestedSegments = m_segmentsCombo->currentData().toInt();

    return item;
}

void NewDownloadDialog::onStartClicked() {
    m_startImmediately = true;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, true);
    accept();
}

void NewDownloadDialog::onLaterClicked() {
    m_startImmediately = false;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, false);
    accept();
}
