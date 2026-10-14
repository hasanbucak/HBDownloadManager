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
#include "test_guard.h"

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
    void testCancelCompletedItemLeavesItCompleted();
    void testRemovePausedItemDeletesPart();
    void testSafeFileRemovalCancelledItemDeletesPart();
    void testRemoveErrorItemDeletesPart();
    void testCompletedFinalFileOutsideFolderIsKept();
    void testRemoveCompletedWithoutDeleteFileKeepsFinalFile();
    void testDuplicateInFlightLinkReturnsExistingId();
    void testSameLinkAllowedAgainWhenNotInFlight();
    void testDeleteFileKeepsFileSharedWithAnotherEntry();
    void testRemoveUnfinishedVideoCleansYtDlpFiles();
};

void TestDownloadLifecycle::initTestCase() {
    // Must run before the DownloadManager singleton (and its QSettings / downloads.json) is touched
    QString why;
    QVERIFY2(TestGuard::enterSandbox(&why), qPrintable(why));
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

void TestDownloadLifecycle::testCancelCompletedItemLeavesItCompleted() {
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "completed-cancel-test-id";
    item.m_fileName = "completed.zip";
    item.m_state = DownloadStateCompleted;
    dm.addDownload(item, false);
    dm.findItem(item.m_id)->m_state = DownloadStateCompleted;

    DownloadItem *found = dm.findItem(item.m_id);
    QVERIFY(found != NULL);
    QCOMPARE(found->m_state, DownloadStateCompleted);

    // D2: cancelDownload on a Completed item leaves it Completed
    dm.cancelDownload(item.m_id);

    found = dm.findItem(item.m_id);
    QVERIFY(found != NULL);
    QCOMPARE(found->m_state, DownloadStateCompleted);

    // Clean up
    dm.removeDownload(item.m_id, false);
}

void TestDownloadLifecycle::testRemovePausedItemDeletesPart() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "paused-delete-part-id";
    item.m_fileName = "paused.zip";
    item.m_directory = tempDir.path();
    item.m_state = DownloadStatePaused;

    QString partPath = item.partFilePath();
    {
        QFile partFile(partPath);
        QVERIFY(partFile.open(QIODevice::WriteOnly));
        partFile.write("RESUMABLE DATA OF AN ITEM THAT IS ABOUT TO LEAVE THE LIST");
        partFile.close();
    }
    QVERIFY(QFile::exists(partPath));

    dm.addDownload(item, false);

    // E10: once the item leaves the list nothing can resume or clean its .part, so it is always
    // removed, whatever the state and whatever deleteFile says
    dm.removeDownload(item.m_id, false);

    QVERIFY(dm.findItem(item.m_id) == NULL);
    QVERIFY(!QFile::exists(partPath));
}

void TestDownloadLifecycle::testSafeFileRemovalCancelledItemDeletesPart() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "cancelled-delete-part-id";
    item.m_fileName = "cancelled.zip";
    item.m_directory = tempDir.path();
    item.m_state = DownloadStateCancelled;

    QString partPath = item.partFilePath();
    {
        QFile partFile(partPath);
        QVERIFY(partFile.open(QIODevice::WriteOnly));
        partFile.write("CANCELLED DISCARDABLE DATA");
        partFile.close();
    }
    QVERIFY(QFile::exists(partPath));

    dm.addDownload(item, false);
    dm.findItem(item.m_id)->m_state = DownloadStateCancelled;

    // removeDownload on a Cancelled item deletes .part even if deleteFile = false
    dm.removeDownload(item.m_id, false);

    QVERIFY(!QFile::exists(partPath));
}

void TestDownloadLifecycle::testRemoveErrorItemDeletesPart() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    DownloadManager &dm = DownloadManager::instance();

    DownloadItem item;
    item.m_id = "error-delete-part-id";
    item.m_fileName = "broken.zip";
    item.m_directory = tempDir.path();
    dm.addDownload(item, false);
    DownloadItem *added = dm.findItem(item.m_id);
    QVERIFY(added != NULL);
    added->m_state = DownloadStateError;

    const QString partPath = item.partFilePath();
    {
        QFile partFile(partPath);
        QVERIFY(partFile.open(QIODevice::WriteOnly));
        partFile.write("PART OF A FAILED DOWNLOAD");
        partFile.close();
    }

    dm.removeDownload(item.m_id, true);
    QVERIFY(!QFile::exists(partPath));
    QVERIFY(dm.findItem(item.m_id) == NULL);
}

void TestDownloadLifecycle::testCompletedFinalFileOutsideFolderIsKept() {
    // E11: a stored final path that is not inside the item's own folder is never deleted
    QTemporaryDir itemDir;
    QTemporaryDir otherDir;
    QVERIFY(itemDir.isValid());
    QVERIFY(otherDir.isValid());

    const QString outsidePath = otherDir.filePath("must_survive.bin");
    {
        QFile f(outsidePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("NOT YOURS TO DELETE");
        f.close();
    }

    DownloadManager &dm = DownloadManager::instance();
    DownloadItem item;
    item.m_id = "completed-outside-id";
    item.m_fileName = "must_survive.bin";
    item.m_directory = itemDir.path();
    item.m_finalPath = outsidePath;
    dm.addDownload(item, false);
    dm.findItem(item.m_id)->m_state = DownloadStateCompleted;

    dm.removeDownload(item.m_id, true);

    QVERIFY(dm.findItem(item.m_id) == NULL);
    QVERIFY2(QFile::exists(outsidePath), "a file outside the item's folder must not be deleted");
}

void TestDownloadLifecycle::testRemoveCompletedWithoutDeleteFileKeepsFinalFile() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString finalPath = tempDir.filePath("keep_me.zip");
    {
        QFile f(finalPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("COMPLETED DATA");
        f.close();
    }

    DownloadManager &dm = DownloadManager::instance();
    DownloadItem item;
    item.m_id = "completed-keep-id";
    item.m_fileName = "keep_me.zip";
    item.m_directory = tempDir.path();
    item.m_finalPath = finalPath;
    dm.addDownload(item, false);
    dm.findItem(item.m_id)->m_state = DownloadStateCompleted;

    dm.removeDownload(item.m_id, false);
    QVERIFY(QFile::exists(finalPath));
}

// DownloadManager::cleanupOrphanParts() (sweeps .dm-<id>.part files of unknown ids older than 24 h in
// the folders of loaded items) runs only inside the singleton's constructor, which cannot be
// re-run in one process; there is no test for it here.

QTEST_MAIN(TestDownloadLifecycle)
void TestDownloadLifecycle::testDuplicateInFlightLinkReturnsExistingId() {
    DownloadManager &dm = DownloadManager::instance();
    DownloadItem item(QUrl(QStringLiteral("http://127.0.0.1:9/dup-inflight.bin")));
    item.m_kind = DownloadKindFile;
    const QString first = dm.addDownload(item, true);   // queued or running: in flight
    const int count = dm.items().size();

    DownloadItem again(QUrl(QStringLiteral("http://127.0.0.1:9/dup-inflight.bin")));
    again.m_kind = DownloadKindFile;
    const QString second = dm.addDownload(again, true);
    QCOMPARE(second, first);
    QCOMPARE(dm.items().size(), count);

    dm.removeDownload(first, false);
}

void TestDownloadLifecycle::testSameLinkAllowedAgainWhenNotInFlight() {
    DownloadManager &dm = DownloadManager::instance();
    DownloadItem item(QUrl(QStringLiteral("http://127.0.0.1:9/dup-later.bin")));
    item.m_kind = DownloadKindFile;
    const QString first = dm.addDownload(item, false);  // "download later": paused, not in flight

    DownloadItem again(QUrl(QStringLiteral("http://127.0.0.1:9/dup-later.bin")));
    again.m_kind = DownloadKindFile;
    const QString second = dm.addDownload(again, false);
    QVERIFY(second != first);

    dm.removeDownload(first, false);
    dm.removeDownload(second, false);
}

void TestDownloadLifecycle::testDeleteFileKeepsFileSharedWithAnotherEntry() {
    DownloadManager &dm = DownloadManager::instance();
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString shared = QDir(dir.path()).filePath(QStringLiteral("video.mp4"));
    QFile f(shared);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("data");
    f.close();

    QStringList ids;
    for (int i = 0; i < 2; ++i) {
        DownloadItem item(QUrl(QStringLiteral("http://127.0.0.1:9/v%1").arg(i)), dir.path());
        item.m_kind = DownloadKindVideo;
        item.m_fileName = QStringLiteral("video.mp4");
        item.m_finalPath = shared;
        ids << dm.addDownload(item, false);
        DownloadItem *added = dm.findItem(ids.last());
        QVERIFY(added != NULL);
        added->m_state = DownloadStateCompleted;
    }

    dm.removeDownload(ids.at(0), true);
    QVERIFY2(QFile::exists(shared), "the other entry still points to this file");
    dm.removeDownload(ids.at(1), true);
    QVERIFY(!QFile::exists(shared));
}

void TestDownloadLifecycle::testRemoveUnfinishedVideoCleansYtDlpFiles() {
    DownloadManager &dm = DownloadManager::instance();
    QTemporaryDir dir;
    QTemporaryDir outsideDir;
    QVERIFY(dir.isValid() && outsideDir.isValid());
    const QDir d(dir.path());
    // Brackets on purpose: a name filter would read "[360p]" as a character class
    const QStringList leftovers = QStringList()
        << QStringLiteral("Clip [360p].f0.mp4")
        << QStringLiteral("Clip [360p].f0.mp4.part")
        << QStringLiteral("Clip [360p].f1.m4a.part")
        << QStringLiteral("Clip [360p].f1.m4a.part-Frag3.part")
        << QStringLiteral("Clip [360p].f1.m4a.ytdl")
        << QStringLiteral("Clip [360p].temp.mp4");
    const QStringList keep = QStringList()
        << QStringLiteral("other.mp4")
        << QStringLiteral("Clip [360p].f0.mp4.notes.txt")
        << QStringLiteral("Clip [360p].final.pdf")
        << QStringLiteral("Clip [360p].f2.txt")
        << QStringLiteral("my.file.mp4.part");
    QStringList all = leftovers + keep;
    for (int i = 0; i < all.size(); ++i) {
        QFile f(d.filePath(all.at(i)));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    // A path outside the download folder in the list must never be touched
    const QString outsideFile = QDir(outsideDir.path()).filePath(QStringLiteral("Clip.f9.mp4"));
    {
        QFile f(outsideFile);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }

    DownloadItem item(QUrl(QStringLiteral("http://127.0.0.1:9/clip.mpd")), dir.path());
    item.m_kind = DownloadKindVideo;
    // The audio format (.f1) is NOT recorded: yt-dlp was stopped before its "Destination" line
    // arrived. It must still go, as a sibling format file of the recorded .f0.
    item.m_tempPaths << d.filePath(QStringLiteral("Clip [360p].f0.mp4"))
                     << d.filePath(QStringLiteral("Clip [360p].mp4"))
                     << outsideFile;
    const QString id = dm.addDownload(item, false);
    dm.removeDownload(id, false);

    for (int i = 0; i < leftovers.size(); ++i) {
        QVERIFY2(!QFile::exists(d.filePath(leftovers.at(i))), qPrintable(leftovers.at(i)));
    }
    for (int i = 0; i < keep.size(); ++i) {
        QVERIFY2(QFile::exists(d.filePath(keep.at(i))), qPrintable(keep.at(i)));
    }
    QVERIFY(QFile::exists(outsideFile));
}

#include "test_download_lifecycle.moc"
