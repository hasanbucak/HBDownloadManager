#include "DownloadDetailDialog.h"
#include "../core/DownloadManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QHeaderView>
#include <QGroupBox>

DownloadDetailDialog::DownloadDetailDialog(const QString &downloadId, QWidget *parent)
    : QDialog(parent),
      m_downloadId(downloadId),
      m_fileNameLabel(new QLabel(this)),
      m_urlLabel(new QLabel(this)),
      m_stateLabel(new QLabel(this)),
      m_sizeLabel(new QLabel(this)),
      m_speedLabel(new QLabel(this)),
      m_etaLabel(new QLabel(this)),
      m_mainProgressBar(new QProgressBar(this)),
      m_segmentBarsContainer(NULL),
      m_segmentsTable(new QTableWidget(this)),
      m_pauseResumeBtn(new QPushButton(tr("Duraklat"), this)),
      m_cancelBtn(new QPushButton(tr("İptal"), this)),
      m_autoCloseCheck(new QCheckBox(tr("İndirme bittiğinde pencereyi kapat"), this)) {
    setupUi();

    DownloadItem *item = DownloadManager::instance().findItem(m_downloadId);
    if (item != NULL) {
        updateUiWithItem(*item);
    }

    connect(&DownloadManager::instance(), SIGNAL(atItemUpdated(DownloadItem)),
            this, SLOT(onItemUpdated(DownloadItem)));
    connect(&DownloadManager::instance(), &DownloadManager::atItemRemoved, this, [this](const QString &id) {
        if (id == m_downloadId) {
            reject();
        }
    });
    connect(m_pauseResumeBtn, SIGNAL(clicked()), this, SLOT(onPauseResumeClicked()));
    connect(m_cancelBtn, SIGNAL(clicked()), this, SLOT(onCancelClicked()));
}

DownloadDetailDialog::~DownloadDetailDialog() {
}

void DownloadDetailDialog::setupUi() {
    setWindowTitle(tr("İndirme Durumu"));
    resize(640, 480);

    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    // Üst Bilgiler
    QGroupBox *infoGroup = new QGroupBox(tr("Dosya Bilgileri"), this);
    QFormLayout *formLayout = new QFormLayout(infoGroup);
    m_stateLabel->setWordWrap(true);
    formLayout->addRow(tr("Dosya:"), m_fileNameLabel);
    formLayout->addRow(tr("Bağlantı:"), m_urlLabel);
    formLayout->addRow(tr("Durum:"), m_stateLabel);
    formLayout->addRow(tr("Boyut:"), m_sizeLabel);

    QHBoxLayout *speedEtaLayout = new QHBoxLayout();
    speedEtaLayout->addWidget(m_speedLabel);
    speedEtaLayout->addSpacing(20);
    speedEtaLayout->addWidget(m_etaLabel);
    speedEtaLayout->addStretch();
    formLayout->addRow(tr("Hız / Kalan:"), speedEtaLayout);

    mainLayout->addWidget(infoGroup);

    // Ana İlerleme Çubuğu
    m_mainProgressBar->setRange(0, 100);
    m_mainProgressBar->setTextVisible(true);
    m_mainProgressBar->setStyleSheet(
        "QProgressBar { border: 1px solid #444; border-radius: 4px; text-align: center; height: 22px; background: #222; color: #fff; font-weight: bold; }"
        "QProgressBar::chunk { background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 #4CAF50, stop:1 #2E7D32); border-radius: 3px; }"
    );
    mainLayout->addWidget(m_mainProgressBar);

    // Segmentler Grubu (IDM Görselleştirme)
    QGroupBox *segmentsGroup = new QGroupBox(tr("Bağlantı Parçaları (Segment İlerlemesi)"), this);
    QVBoxLayout *segLayout = new QVBoxLayout(segmentsGroup);

    m_segmentBarsContainer = new QWidget(this);
    QHBoxLayout *barsHBox = new QHBoxLayout(m_segmentBarsContainer);
    barsHBox->setContentsMargins(0, 0, 0, 0);
    barsHBox->setSpacing(4);
    segLayout->addWidget(m_segmentBarsContainer);

    // Segment Tablosu
    m_segmentsTable->setColumnCount(5);
    QStringList headers;
    headers << tr("No") << tr("Başlangıç") << tr("Bitiş") << tr("İndirilen") << tr("İlerleme");
    m_segmentsTable->setHorizontalHeaderLabels(headers);
    m_segmentsTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_segmentsTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_segmentsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    segLayout->addWidget(m_segmentsTable);

    mainLayout->addWidget(segmentsGroup);

    // Alt Butonlar
    QHBoxLayout *bottomLayout = new QHBoxLayout();
    bottomLayout->addWidget(m_autoCloseCheck);
    bottomLayout->addStretch();
    bottomLayout->addWidget(m_pauseResumeBtn);
    bottomLayout->addWidget(m_cancelBtn);

    mainLayout->addLayout(bottomLayout);
}

void DownloadDetailDialog::onItemUpdated(const DownloadItem &item) {
    if (item.m_id != m_downloadId) return;
    updateUiWithItem(item);

    if (item.m_state == DownloadStateCompleted && m_autoCloseCheck->isChecked()) {
        accept();
    }
}

void DownloadDetailDialog::updateUiWithItem(const DownloadItem &item) {
    setWindowTitle(QString("[%1] %2").arg(item.formattedProgress(), item.m_fileName));

    m_fileNameLabel->setText(item.m_fileName);
    m_urlLabel->setText(item.m_url.toString());
    if (item.m_state == DownloadStateError && !item.m_errorMessage.isEmpty()) {
        m_stateLabel->setText(QString("%1: %2").arg(item.stateString(), item.m_errorMessage));
    } else {
        m_stateLabel->setText(item.stateString());
    }
    m_sizeLabel->setText(item.formattedSize());
    m_speedLabel->setText(tr("Hız: %1").arg(item.formattedSpeed()));
    m_etaLabel->setText(tr("Kalan Süre: %1").arg(item.formattedEta()));

    int pct = static_cast<int>(item.progress() * 100.0);
    m_mainProgressBar->setValue(pct);

    if (item.isActive()) {
        m_pauseResumeBtn->setText(tr("Duraklat"));
        m_pauseResumeBtn->setEnabled(true);
    } else if (item.m_state == DownloadStatePaused || item.m_state == DownloadStateError) {
        m_pauseResumeBtn->setText(tr("Devam Et"));
        m_pauseResumeBtn->setEnabled(true);
    } else {
        m_pauseResumeBtn->setEnabled(false);
    }

    // Segment Çubuklarını ve Tablosunu Güncelle
    int segCount = item.m_segments.size();
    if (segCount > 0) {
        QHBoxLayout *barsHBox = qobject_cast<QHBoxLayout*>(m_segmentBarsContainer->layout());

        while (m_segmentBars.size() < segCount) {
            QProgressBar *pb = new QProgressBar(m_segmentBarsContainer);
            pb->setRange(0, 100);
            pb->setTextVisible(false);
            pb->setFixedHeight(14);
            pb->setStyleSheet(
                "QProgressBar { border: 1px solid #555; background: #333; border-radius: 2px; }"
                "QProgressBar::chunk { background: #2196F3; }"
            );
            barsHBox->addWidget(pb);
            m_segmentBars.append(pb);
        }

        while (m_segmentBars.size() > segCount) {
            delete m_segmentBars.takeLast();
        }

        m_segmentsTable->setRowCount(segCount);
        for (int i = 0; i < segCount; ++i) {
            const SegmentInfo &seg = item.m_segments.at(i);
            int segPct = static_cast<int>(seg.progress() * 100.0);
            m_segmentBars.at(i)->setValue(segPct);

            m_segmentsTable->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
            m_segmentsTable->setItem(i, 1, new QTableWidgetItem(QString::number(seg.m_start)));
            m_segmentsTable->setItem(i, 2, new QTableWidgetItem(QString::number(seg.m_end)));
            m_segmentsTable->setItem(i, 3, new QTableWidgetItem(QString::number(seg.m_received)));
            m_segmentsTable->setItem(i, 4, new QTableWidgetItem(QString("%1%").arg(segPct)));
        }
    }
}

void DownloadDetailDialog::onPauseResumeClicked() {
    DownloadItem *item = DownloadManager::instance().findItem(m_downloadId);
    if (item == NULL) return;

    if (item->isActive()) {
        DownloadManager::instance().pauseDownload(m_downloadId);
    } else if (item->m_state == DownloadStatePaused || item->m_state == DownloadStateError) {
        DownloadManager::instance().resumeDownload(m_downloadId);
    }
}

void DownloadDetailDialog::onCancelClicked() {
    DownloadManager::instance().cancelDownload(m_downloadId);
    reject();
}
