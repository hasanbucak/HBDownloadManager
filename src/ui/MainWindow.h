#pragma once

#include "../core/DownloadItem.h"
#include <QMainWindow>
#include <QTableWidget>
#include <QListWidget>
#include <QToolBar>
#include <QAction>
#include <QLabel>
#include <QMap>

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = NULL);
    virtual ~MainWindow();

private slots:
    void onNewDownloadClicked();
    void onPauseClicked();
    void onResumeClicked();
    void onRemoveClicked();
    void onClearCompletedClicked();
    void onSettingsClicked();
    void onCategorySelected(int row);
    void onTableDoubleClicked(int row, int column);
    void onTableContextMenu(const QPoint &pos);
    void onInstallHostBridgeClicked();

    void onItemAdded(const DownloadItem &item);
    void onItemUpdated(const DownloadItem &item);
    void onItemRemoved(const QString &id);
    void onUrlDetected(const QUrl &url);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    void setupUi();
    void setupToolBar();
    void setupSidebar();
    void setupTable();
    void setupStatusBar();
    void applyDarkTheme();

    void refreshTable();
    void updateTableRow(int row, const DownloadItem &item);
    QString selectedDownloadId() const;
    int findRowById(const QString &id) const;

    QToolBar *m_toolBar;
    QAction *m_actNew;
    QAction *m_actPause;
    QAction *m_actResume;
    QAction *m_actRemove;
    QAction *m_actClearCompleted;
    QAction *m_actSettings;

    QListWidget *m_categoryList;
    QTableWidget *m_downloadTable;

    QLabel *m_statusSpeedLabel;
    QLabel *m_statusActiveLabel;
    QLabel *m_statusServerLabel;

    int m_selectedCategory;
    QMap<QString, QString> m_rowToIdMap;
};
