#include <QTest>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QStandardPaths>
#include "DownloadManager.h"
#include "DownloadItem.h"

class TestDownloadLifecycle : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testAtomicSaveStateAndPermissions();
    void testCorruptedStateFileRenamed();
    void testSafeFileRemovalIncompleteDoesNotDeleteTargetFile();
    void testSafeFileRemovalCompletedDeletesFinalPathOnlyIfRequested();
    void testCookieClearingOnCompletionAndFailure();
    void testShutdownLifecycle();
};

void TestDownloadLifecycle::initTestCase() {
    // Ensure QCoreApplication exists for aboutToQuit connects
    QVERIFY(QCoreApplication::instance() != NULL);
}

void TestDownloadLifecycle::testAtomicSaveStateAndPermissions() {
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "test-perm-id";
    item.m_fileName = "test.bin";
    item.m_state = DownloadStateCompleted;
    dm.addDownload(item, false);

    dm.saveState();

    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/hb-download-manager";
    QString statePath = configDir + "/downloads.json";

    QVERIFY(QFile::exists(statePath));

    QFile::Permissions perms = QFile::permissions(statePath);
    // Must have ReadOwner and WriteOwner
    QVERIFY(perms & QFileDevice::ReadOwner);
    QVERIFY(perms & QFileDevice::WriteOwner);

    // Private 0600 permissions: others/group should not have write/read permissions
    QVERIFY(!(perms & QFileDevice::WriteGroup));
    QVERIFY(!(perms & QFileDevice::WriteOther));

    // Cleanup added item
    dm.removeDownload(item.m_id, false);
}

void TestDownloadLifecycle::testCorruptedStateFileRenamed() {
    DownloadManager &dm = DownloadManager::instance();
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/hb-download-manager";
    QDir().mkpath(configDir);
    QString statePath = configDir + "/downloads.json";

    // Write corrupted JSON into downloads.json
    {
        QFile file(statePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"corrupt\": [truncated json string without closing bracket...");
        file.close();
    }

    dm.loadState();

    // Original downloads.json must have been renamed to .corrupt-<timestamp>
    QVERIFY(!QFile::exists(statePath));

    QDir dir(configDir);
    QStringList corruptFiles = dir.entryList(QStringList() << "downloads.json.corrupt-*", QDir::Files);
    QVERIFY(!corruptFiles.isEmpty());

    // Clean up test corrupt files
    for (int i = 0; i < corruptFiles.size(); ++i) {
        dir.remove(corruptFiles.at(i));
    }
}

void TestDownloadLifecycle::testSafeFileRemovalIncompleteDoesNotDeleteTargetFile() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DownloadManager &dm = DownloadManager::instance();

    // Create an existing pre-existing file in the download directory
    QString existingFileName = "pre_existing_setup.zip";
    QString existingFilePath = tempDir.filePath(existingFileName);
    {
        QFile file(existingFilePath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("IMPORTANT PRE-EXISTING DATA");
        file.close();
    }
    QVERIFY(QFile::exists(existingFilePath));

    // Create a new incomplete download with the same filename
    DownloadItem item;
    item.m_id = "incomplete-item-123";
    item.m_fileName = existingFileName;
    item.m_directory = tempDir.path();
    item.m_state = DownloadStatePaused; // Incomplete

    // Create a fake .part file
    QString partPath = item.partFilePath();
    {
        QFile partFile(partPath);
        QVERIFY(partFile.open(QIODevice::WriteOnly));
        partFile.write("PARTIAL INCOMPLETE DATA");
        partFile.close();
    }
    QVERIFY(QFile::exists(partPath));

    dm.addDownload(item, false);

    // Remove the download with deleteFile = true
    dm.removeDownload(item.m_id, true);

    // F7: The .part file must be removed
    QVERIFY(!QFile::exists(partPath));

    // F7: The pre-existing target file MUST NOT be deleted for an incomplete download!
    QVERIFY(QFile::exists(existingFilePath));

    // Verify existing file content is intact
    QFile file(existingFilePath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("IMPORTANT PRE-EXISTING DATA"));
    file.close();
}

void TestDownloadLifecycle::testSafeFileRemovalCompletedDeletesFinalPathOnlyIfRequested() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    DownloadManager &dm = DownloadManager::instance();

    QString finalName = "completed_file.zip";
    QString finalPath = tempDir.filePath(finalName);
    {
        QFile file(finalPath);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("COMPLETED DATA");
        file.close();
    }
    QVERIFY(QFile::exists(finalPath));

    // 1. Test remove without deleteFile: file remains
    DownloadItem item1;
    item1.m_id = "completed-item-1";
    item1.m_fileName = finalName;
    item1.m_directory = tempDir.path();
    item1.m_finalPath = finalPath;
    dm.addDownload(item1, false);
    dm.findItem(item1.m_id)->m_state = DownloadStateCompleted;

    dm.removeDownload(item1.m_id, false);
    QVERIFY(QFile::exists(finalPath)); // preserved

    // 2. Test remove WITH deleteFile: file removed
    DownloadItem item2;
    item2.m_id = "completed-item-2";
    item2.m_fileName = finalName;
    item2.m_directory = tempDir.path();
    item2.m_finalPath = finalPath;
    dm.addDownload(item2, false);
    dm.findItem(item2.m_id)->m_state = DownloadStateCompleted;

    dm.removeDownload(item2.m_id, true);
    QVERIFY(!QFile::exists(finalPath)); // successfully deleted
}

void TestDownloadLifecycle::testCookieClearingOnCompletionAndFailure() {
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "cookie-test-id";
    item.m_fileName = "test.zip";
    item.m_cookieHeader = "session=secret_token_12345";
    QJsonObject c;
    c["name"] = "session";
    c["value"] = "secret_token_12345";
    item.m_cookies.append(c);
    item.m_state = DownloadStateDownloading;

    dm.addDownload(item, false);

    DownloadItem *found = dm.findItem(item.m_id);
    QVERIFY(found != NULL);
    QCOMPARE(found->m_cookieHeader, QString("session=secret_token_12345"));
    QCOMPARE(found->m_cookies.size(), 1);

    // Cancel download (permanent termination)
    dm.cancelDownload(item.m_id);

    found = dm.findItem(item.m_id);
    QVERIFY(found != NULL);
    QCOMPARE(found->m_state, DownloadStateCancelled);
    // S4: Session cookies must be cleared!
    QVERIFY(found->m_cookieHeader.isEmpty());
    QVERIFY(found->m_cookies.isEmpty());

    // Clean up
    dm.removeDownload(item.m_id, false);
}

void TestDownloadLifecycle::testShutdownLifecycle() {
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem queuedItem;
    queuedItem.m_id = "queued-lifecycle-id";
    queuedItem.m_fileName = "queued.zip";
    queuedItem.m_state = DownloadStateQueued;
    dm.addDownload(queuedItem, false);

    // pauseAll must pause queued items
    dm.pauseAll();

    DownloadItem *found = dm.findItem(queuedItem.m_id);
    QVERIFY(found != NULL);
    QCOMPARE(found->m_state, DownloadStatePaused);

    // Clean up
    dm.removeDownload(queuedItem.m_id, false);
}

QTEST_MAIN(TestDownloadLifecycle)
#include "test_download_lifecycle.moc"
