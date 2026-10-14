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
#include <QStorageInfo>
#include <QDir>
#include <QFile>

NewDownloadDialog::NewDownloadDialog(const QUrl &url, QWidget *parent)
    : QDialog(parent),
      m_urlEdit(new QLineEdit(this)),
      m_fileNameEdit(new QLineEdit(this)),
      m_directoryEdit(new QLineEdit(this)),
      m_segmentsCombo(new QComboBox(this)),
      m_fileRadio(new QRadioButton(tr("Normal Dosya"), this)),
      m_videoRadio(new QRadioButton(tr("Video / Medya Akışı (yt-dlp)"), this)),
      m_qualityCombo(new QComboBox(this)),
      m_qualityLabel(new QLabel(tr("Kalite:"), this)),
      m_toolWarnLabel(new QLabel(this)),
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
        m_autoName = name;

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
    int segIndex = -1;
    for (int i = 0; i < m_segmentsCombo->count(); ++i) {
        if (m_segmentsCombo->itemData(i).toInt() == defSegs) {
            segIndex = i;
            break;
        }
    }
    if (segIndex < 0 && defSegs >= 1 && defSegs <= 16) {
        // A value from Settings that is not a preset (e.g. 6): add it in order so the setting is honoured
        segIndex = m_segmentsCombo->count();
        for (int i = 0; i < m_segmentsCombo->count(); ++i) {
            if (m_segmentsCombo->itemData(i).toInt() > defSegs) {
                segIndex = i;
                break;
            }
        }
        m_segmentsCombo->insertItem(segIndex, QString::number(defSegs), defSegs);
    }
    if (segIndex >= 0) {
        m_segmentsCombo->setCurrentIndex(segIndex);
    }
    updateKindUi();

    connect(m_urlEdit, SIGNAL(textChanged(QString)), this, SLOT(onUrlTextChanged(QString)));
    connect(m_fileNameEdit, SIGNAL(textEdited(QString)), this, SLOT(onFileNameTextEdited(QString)));
    connect(m_videoRadio, SIGNAL(toggled(bool)), this, SLOT(updateKindUi()));
    connect(m_browseBtn, SIGNAL(clicked()), this, SLOT(onBrowseClicked()));
    connect(m_startBtn, SIGNAL(clicked()), this, SLOT(onStartClicked()));
    connect(m_laterBtn, SIGNAL(clicked()), this, SLOT(onLaterClicked()));
    connect(m_cancelBtn, SIGNAL(clicked()), this, SLOT(reject()));
}

NewDownloadDialog::~NewDownloadDialog() {
}

void NewDownloadDialog::setupUi() {
    setWindowTitle(tr("Yeni İndirme Başlat"));
    resize(520, 320);

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
    m_segmentsCombo->addItem(tr("8 (Önerilen)"), 8);
    m_segmentsCombo->addItem(tr("16 (Maksimum)"), 16);
    m_segmentsCombo->setCurrentIndex(3);
    formLayout->addRow(tr("Bağlantı Sayısı:"), m_segmentsCombo);

    QHBoxLayout *kindLayout = new QHBoxLayout();
    kindLayout->addWidget(m_fileRadio);
    kindLayout->addWidget(m_videoRadio);
    kindLayout->addStretch();
    formLayout->addRow(tr("İndirme Türü:"), kindLayout);

    m_qualityCombo->addItem(tr("En Yüksek"), "best");
    m_qualityCombo->addItem("1080p", "1080");
    m_qualityCombo->addItem("720p", "720");
    m_qualityCombo->addItem("480p", "480");
    m_qualityCombo->addItem(tr("Sadece Ses"), "audio");
    formLayout->addRow(m_qualityLabel, m_qualityCombo);

    m_toolWarnLabel->setWordWrap(true);
    m_toolWarnLabel->setStyleSheet("color: #ffd166;");
    m_toolWarnLabel->setText(tr("yt-dlp bulunamadı: video indirmeleri çalışmaz. yt-dlp paketini kurun."));
    m_toolWarnLabel->hide();

    mainLayout->addLayout(formLayout);
    mainLayout->addWidget(m_toolWarnLabel);

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
            m_autoName = n;
            m_isFileNameAuto = true;
        }
        if (MediaHosts::isStreamingSite(u.host())) {
            m_videoRadio->setChecked(true);
        }
    }
    updateKindUi();  // the radio may already have been on Video: no toggled() to clear the name
}

void NewDownloadDialog::onFileNameTextEdited(const QString &text) {
    Q_UNUSED(text);
    m_isFileNameAuto = false;
}

void NewDownloadDialog::updateKindUi() {
    const bool video = m_videoRadio->isChecked();
    m_qualityLabel->setVisible(video);
    m_qualityCombo->setVisible(video);
    // yt-dlp names the file from the video title: an empty field with a hint, not a stale "watch".
    // The file name is kept aside and comes back when the user switches to a regular file.
    if (video) {
        if (!m_fileNameEdit->text().isEmpty()) {
            m_parkedFileName = m_fileNameEdit->text();
            m_fileNameEdit->clear();
        }
        m_fileNameEdit->setPlaceholderText(tr("Video başlığından otomatik alınır"));
    } else {
        m_fileNameEdit->setPlaceholderText(QString());
        if (m_fileNameEdit->text().isEmpty() && !m_parkedFileName.isEmpty()) {
            m_fileNameEdit->setText(m_parkedFileName);
        }
    }
    m_fileNameEdit->setEnabled(!video);
    m_toolWarnLabel->setVisible(video && !VideoDownloader::isAvailable());
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

    if (m_videoRadio->isChecked()) {
        const QString quality = m_qualityCombo->currentData().toString();
        item.m_kind = (quality == "audio" ? DownloadKindAudio : DownloadKindVideo);
        item.m_quality = quality;
        item.m_fileNameExplicit = false;
    } else {
        item.m_kind = DownloadKindFile;
        const QString name = m_fileNameEdit->text().trimmed();
        if (!name.isEmpty()) {
            item.m_fileName = PathUtils::sanitizeFileName(name);
        }
        // Only a name the user typed over the URL suggestion beats the server's Content-Disposition name
        item.m_fileNameExplicit = (!m_isFileNameAuto && !name.isEmpty() && name != m_autoName);
    }
    item.m_requestedSegments = m_segmentsCombo->currentData().toInt();

    return item;
}

bool NewDownloadDialog::validateInputs() {
    QUrl url(m_urlEdit->text().trimmed());
    if (!url.isValid() || (url.scheme() != "http" && url.scheme() != "https") || url.host().isEmpty()) {
        QMessageBox::warning(this, tr("Geçersiz Bağlantı"), tr("Lütfen geçerli bir http veya https bağlantısı girin."));
        return false;
    }

    const QString dir = m_directoryEdit->text().trimmed();
    if (dir.isEmpty()) {
        QMessageBox::warning(this, tr("Geçersiz Klasör"), tr("Lütfen bir kayıt klasörü seçin."));
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

    // Free space: the size is unknown before the first request, so only warn when the disk is nearly full
    const qint64 kLowSpaceBytes = 100LL * 1024 * 1024;
    QStorageInfo storage(dir);
    if (storage.isValid() && storage.isReady()) {
        const qint64 avail = storage.bytesAvailable();
        if (avail >= 0 && avail < kLowSpaceBytes) {
            const QString availStr = QString::number(avail / (1024.0 * 1024.0), 'f', 1);
            QMessageBox::StandardButton r = QMessageBox::warning(
                this, tr("Disk Alanı Az"),
                tr("Seçilen konumda yalnızca %1 MB boş alan var. Yine de devam edilsin mi?").arg(availStr),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (r != QMessageBox::Yes) {
                return false;
            }
        }
    }

    // File exists: the engine never overwrites, it saves under the next free name.
    // Video items are named by yt-dlp, so there is no name to compare.
    DownloadItem item = downloadItem();
    if (item.m_kind != DownloadKindFile) {
        return true;
    }
    const QString wanted = QDir(dir).filePath(item.m_fileName);
    bool clash = QFileInfo::exists(wanted);
    if (!clash) {
        const QList<DownloadItem> &list = DownloadManager::instance().items();
        for (int i = 0; i < list.size() && !clash; ++i) {
            const DownloadItem &other = list.at(i);
            if (other.m_state != DownloadStateCompleted && other.m_state != DownloadStateCancelled &&
                QDir::cleanPath(other.m_directory) == QDir::cleanPath(dir) && other.m_fileName == item.m_fileName) {
                clash = true;
            }
        }
    }
    if (clash) {
        const QString alt = QFileInfo(PathUtils::uniqueTargetPath(dir, item.m_fileName, item.m_id)).fileName();
        QMessageBox::StandardButton r = QMessageBox::question(
            this, tr("Dosya Zaten Var"),
            tr("\"%1\" adlı bir dosya zaten var veya aynı ada sahip bir indirme sürüyor. "
               "Mevcut dosya silinmez; yeni indirme \"%2\" adıyla kaydedilebilir. Devam edilsin mi?")
                .arg(item.m_fileName, alt),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Yes);
        if (r != QMessageBox::Yes) {
            return false;
        }
    }
    return true;
}

void NewDownloadDialog::onStartClicked() {
    if (!validateInputs()) {
        return;
    }

    m_startImmediately = true;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, true);
    accept();
}

void NewDownloadDialog::onLaterClicked() {
    if (!validateInputs()) {
        return;
    }

    m_startImmediately = false;
    DownloadItem item = downloadItem();
    emit atDownloadConfirmed(item, false);
    accept();
}
