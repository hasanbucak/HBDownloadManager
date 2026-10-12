#include "NewDownloadDialog.h"
#include "PathUtils.h"
#include "MediaHosts.h"
#include "../core/DownloadManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QGroupBox>
#include <QMessageBox>

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
      m_startImmediately(true),
      m_isFileNameAuto(true) {
    setupUi();

    if (url.isValid()) {
        m_urlEdit->setText(url.toString());
        QString name = QFileInfo(url.path()).fileName();
        if (name.isEmpty()) name = "download";
        m_fileNameEdit->setText(name);

        if (MediaHosts::isStreamingSite(url.host())) {
            m_videoRadio->setChecked(true);
        } else {
            m_fileRadio->setChecked(true);
        }
    } else {
        m_fileRadio->setChecked(true);
    }

    m_directoryEdit->setText(DownloadManager::instance().defaultDirectory());

    // U7: Pre-select segment combo index based on defaultSegments()
    int defSegs = DownloadManager::instance().defaultSegments();
    for (int i = 0; i < m_segmentsCombo->count(); ++i) {
        if (m_segmentsCombo->itemData(i).toInt() == defSegs) {
            m_segmentsCombo->setCurrentIndex(i);
            break;
        }
    }

    connect(m_urlEdit, SIGNAL(textChanged(QString)), this, SLOT(onUrlTextChanged(QString)));
    connect(m_fileNameEdit, SIGNAL(textEdited(QString)), this, SLOT(onFileNameTextEdited(QString)));
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

void NewDownloadDialog::onUrlTextChanged(const QString &text) {
    QUrl u(text.trimmed());
    if (u.isValid() && (u.scheme() == "http" || u.scheme() == "https") && !u.host().isEmpty()) {
        if (m_fileNameEdit->text().isEmpty() || m_isFileNameAuto) {
            QString n = QFileInfo(u.path()).fileName();
            if (n.isEmpty()) n = "download";
            m_fileNameEdit->setText(n);
            m_isFileNameAuto = true;
        }
        if (MediaHosts::isStreamingSite(u.host())) {
            m_videoRadio->setChecked(true);
        }
    }
}

void NewDownloadDialog::onFileNameTextEdited(const QString &text) {
    Q_UNUSED(text);
    m_isFileNameAuto = false;
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
        item.m_fileName = PathUtils::sanitizeFileName(name);
    }
    item.m_kind = (m_videoRadio->isChecked() ? DownloadKindVideo : DownloadKindFile);
    item.m_requestedSegments = m_segmentsCombo->currentData().toInt();

    return item;
}

void NewDownloadDialog::onStartClicked() {
    QUrl url(m_urlEdit->text().trimmed());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()) {
        QMessageBox::warning(this, tr("Geçersiz Bağlantı"), tr("Lütfen geçerli bir http veya https bağlantısı girin."));
        return;
    }

    m_startImmediately = true;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, true);
    accept();
}

void NewDownloadDialog::onLaterClicked() {
    QUrl url(m_urlEdit->text().trimmed());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()) {
        QMessageBox::warning(this, tr("Geçersiz Bağlantı"), tr("Lütfen geçerli bir http veya https bağlantısı girin."));
        return;
    }

    m_startImmediately = false;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, false);
    accept();
}
