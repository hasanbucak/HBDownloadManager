#pragma once

#include "../core/DownloadItem.h"
#include <QDialog>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QCheckBox>
#include <QTableWidget>
#include <QList>

class DownloadDetailDialog : public QDialog {
    Q_OBJECT
public:
    explicit DownloadDetailDialog(const QString &downloadId, QWidget *parent = NULL);
    virtual ~DownloadDetailDialog();

    QString downloadId() const { return m_downloadId; }

public slots:
    void onItemUpdated(const DownloadItem &item);

private slots:
    void onPauseResumeClicked();
    void onCancelClicked();

private:
    void setupUi();
    void updateUiWithItem(const DownloadItem &item);

    QString m_downloadId;
    QLabel *m_fileNameLabel;
    QLabel *m_urlLabel;
    QLabel *m_stateLabel;
    QLabel *m_sizeLabel;
    QLabel *m_speedLabel;
    QLabel *m_etaLabel;
    QProgressBar *m_mainProgressBar;

    QList<QProgressBar*> m_segmentBars;
    QWidget *m_segmentBarsContainer;
    QTableWidget *m_segmentsTable;

    QPushButton *m_pauseResumeBtn;
    QPushButton *m_cancelBtn;
    QCheckBox *m_autoCloseCheck;
};
