#pragma once

#include "../core/DownloadItem.h"
#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QRadioButton>
#include <QPushButton>

class NewDownloadDialog : public QDialog {
    Q_OBJECT
public:
    explicit NewDownloadDialog(const QUrl &url = QUrl(), QWidget *parent = NULL);
    virtual ~NewDownloadDialog();

    DownloadItem downloadItem() const;
    bool shouldStartImmediately() const { return m_startImmediately; }

signals:
    void atDownloadConfirmed(const DownloadItem &item, bool startImmediately);

private slots:
    void onBrowseClicked();
    void onStartClicked();
    void onLaterClicked();
    void onUrlTextChanged(const QString &text);
    void onFileNameTextEdited(const QString &text);

private:
    void setupUi();
    // URL, folder (created if missing, must be writable), free space and "file exists" checks
    bool validateInputs();

    QLineEdit *m_urlEdit;
    QLineEdit *m_fileNameEdit;
    QLineEdit *m_directoryEdit;
    QComboBox *m_segmentsCombo;
    QRadioButton *m_fileRadio;
    QRadioButton *m_videoRadio;
    QPushButton *m_browseBtn;
    QPushButton *m_startBtn;
    QPushButton *m_laterBtn;
    QPushButton *m_cancelBtn;

    bool m_startImmediately;
    bool m_isFileNameAuto;
};
