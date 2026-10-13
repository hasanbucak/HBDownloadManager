#pragma once

#include <QDialog>

class QLineEdit;
class QSpinBox;
class QCheckBox;
class QPushButton;

// U9: settings for the options DownloadManager already persists, plus the browser bridge installer.
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(QWidget *parent = NULL);
    virtual ~SettingsDialog();

signals:
    // The owner (MainWindow) runs the actual host installation
    void atInstallBridgeRequested();

private slots:
    void onBrowseClicked();
    void onOkClicked();

private:
    void setupUi();
    bool validateDirectory(QString *dirOut);

    QLineEdit *m_directoryEdit;
    QPushButton *m_browseBtn;
    QSpinBox *m_segmentsSpin;
    QSpinBox *m_concurrentSpin;
    QSpinBox *m_portSpin;
    QCheckBox *m_clipboardCheck;
    QPushButton *m_installBtn;
    QPushButton *m_okBtn;
    QPushButton *m_cancelBtn;
    quint16 m_initialPort;
};
