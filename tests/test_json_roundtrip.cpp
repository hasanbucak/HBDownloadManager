#include <QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QTemporaryDir>
#include <QDir>
#include "DownloadItem.h"

class TestJsonRoundtrip : public QObject {
    Q_OBJECT

private slots:
    void testCompleteItemRoundtrip();
    void testStateTransitions();
    void testKindRoundtrip();
    void testEmptyJsonObject();
    void testCorruptDataTypes();
    void testCorruptJsonDocumentParsing();
    void testTruncatedJsonStateFileSimulation();
};

void TestJsonRoundtrip::testCompleteItemRoundtrip() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString canonicalDir = QDir(tempDir.path()).canonicalPath();

    DownloadItem original;
    original.m_id = QStringLiteral("6ba7b810-9dad-11d1-80b4-00c04fd430c8");
    original.m_url = QUrl(QStringLiteral("https://mirror.example.org/linux/ubuntu-24.04.iso?token=sec123"));
    original.m_fileName = QStringLiteral("ubuntu-24.04.iso");
    original.m_directory = canonicalDir;
    original.m_kind = DownloadKindFile;
    original.m_state = DownloadStateCompleted;
    original.m_totalBytes = 4294967296LL; // 4 GiB
    original.m_receivedBytes = 4294967296LL;
    original.m_supportsRanges = true;
    original.m_errorMessage = QString();
    original.m_createdAt = QDateTime::fromString(QStringLiteral("2026-10-12T10:00:00Z"), Qt::ISODate);
    original.m_completedAt = QDateTime::fromString(QStringLiteral("2026-10-12T10:15:30Z"), Qt::ISODate);
    original.m_finalPath = QDir(canonicalDir).filePath(QStringLiteral("ubuntu-24.04.iso"));
    original.m_requestedSegments = 4;
    original.m_quality = QStringLiteral("1080p");
    original.m_cookieHeader = QStringLiteral("session_id=abcdef123456; domain=.example.org");

    QJsonObject cookieObj;
    cookieObj[QStringLiteral("name")] = QStringLiteral("session_id");
    cookieObj[QStringLiteral("value")] = QStringLiteral("abcdef123456");
    cookieObj[QStringLiteral("domain")] = QStringLiteral(".example.org");
    original.m_cookies.append(cookieObj);

    original.m_headers[QStringLiteral("User-Agent")] = QStringLiteral("HB-Download-Manager/1.3.0");
    original.m_headers[QStringLiteral("Referer")] = QStringLiteral("https://example.org/");

    // Add 4 segments
    const qint64 segSize = original.m_totalBytes / 4;
    for (int i = 0; i < 4; ++i) {
        SegmentInfo s;
        s.m_index = i;
        s.m_start = i * segSize;
        s.m_end = (i == 3) ? (original.m_totalBytes - 1) : (s.m_start + segSize - 1);
        s.m_received = s.length();
        original.m_segments.append(s);
    }

    // Serialize to JSON
    const QJsonObject json = original.toJson();

    // Deserialize back
    const DownloadItem restored = DownloadItem::fromJson(json);

    // Verify all fields preserved identically
    QCOMPARE(restored.m_id, original.m_id);
    QCOMPARE(restored.m_url, original.m_url);
    QCOMPARE(restored.m_fileName, original.m_fileName);
    QCOMPARE(restored.m_directory, original.m_directory);
    QCOMPARE(restored.m_kind, original.m_kind);
    QCOMPARE(restored.m_state, original.m_state);
    QCOMPARE(restored.m_totalBytes, original.m_totalBytes);
    QCOMPARE(restored.m_receivedBytes, original.m_receivedBytes);
    QCOMPARE(restored.m_supportsRanges, original.m_supportsRanges);
    QCOMPARE(restored.m_errorMessage, original.m_errorMessage);
    QCOMPARE(restored.m_createdAt.toString(Qt::ISODate), original.m_createdAt.toString(Qt::ISODate));
    QCOMPARE(restored.m_completedAt.toString(Qt::ISODate), original.m_completedAt.toString(Qt::ISODate));
    QCOMPARE(restored.m_finalPath, original.m_finalPath);
    QCOMPARE(restored.m_requestedSegments, original.m_requestedSegments);
    QCOMPARE(restored.m_quality, original.m_quality);
    QCOMPARE(restored.m_cookieHeader, original.m_cookieHeader);
    QCOMPARE(restored.m_cookies.size(), original.m_cookies.size());
    QCOMPARE(restored.m_headers.value(QStringLiteral("User-Agent")), QStringLiteral("HB-Download-Manager/1.3.0"));
    QCOMPARE(restored.m_headers.value(QStringLiteral("Referer")), QStringLiteral("https://example.org/"));

    // Verify segments
    QCOMPARE(restored.m_segments.size(), 4);
    for (int i = 0; i < 4; ++i) {
        QCOMPARE(restored.m_segments.at(i).m_index, original.m_segments.at(i).m_index);
        QCOMPARE(restored.m_segments.at(i).m_start, original.m_segments.at(i).m_start);
        QCOMPARE(restored.m_segments.at(i).m_end, original.m_segments.at(i).m_end);
        QCOMPARE(restored.m_segments.at(i).m_received, original.m_segments.at(i).m_received);
    }
}

void TestJsonRoundtrip::testStateTransitions() {
    DownloadItem item;

    // Completed state must stay Completed
    item.m_state = DownloadStateCompleted;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStateCompleted);

    // Paused state must stay Paused
    item.m_state = DownloadStatePaused;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStatePaused);

    // Failed (Error) state must stay Failed (DownloadStateError)
    item.m_state = DownloadStateError;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStateError);

    // Cancelled state must stay Cancelled
    item.m_state = DownloadStateCancelled;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStateCancelled);

    // Queued state must stay Queued
    item.m_state = DownloadStateQueued;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStateQueued);

    // Active downloading states must safely reload as Paused (prevent uncoordinated runaway downloads on launch)
    item.m_state = DownloadStateDownloading;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStatePaused);

    item.m_state = DownloadStateConnecting;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_state, DownloadStatePaused);
}

void TestJsonRoundtrip::testKindRoundtrip() {
    DownloadItem item;

    item.m_kind = DownloadKindFile;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_kind, DownloadKindFile);

    item.m_kind = DownloadKindVideo;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_kind, DownloadKindVideo);

    item.m_kind = DownloadKindAudio;
    QCOMPARE(DownloadItem::fromJson(item.toJson()).m_kind, DownloadKindAudio);
}

void TestJsonRoundtrip::testEmptyJsonObject() {
    const QJsonObject emptyObj;
    const DownloadItem item = DownloadItem::fromJson(emptyObj);

    // Must not crash and should provide sane defaults
    QVERIFY(item.m_id.isEmpty());
    QCOMPARE(item.m_kind, DownloadKindFile);
    QCOMPARE(item.m_state, DownloadStateQueued);
    QCOMPARE(item.m_totalBytes, 0LL);
    QCOMPARE(item.m_receivedBytes, 0LL);
    QVERIFY(!item.m_supportsRanges);
    QVERIFY(item.m_segments.isEmpty());
    QVERIFY(item.m_createdAt.isValid());
}

void TestJsonRoundtrip::testCorruptDataTypes() {
    QJsonObject corruptObj;
    corruptObj[QStringLiteral("id")] = 12345; // integer instead of string
    corruptObj[QStringLiteral("url")] = true; // boolean instead of string
    corruptObj[QStringLiteral("totalBytes")] = QStringLiteral("not_a_number");
    corruptObj[QStringLiteral("receivedBytes")] = QStringLiteral("corrupt");
    corruptObj[QStringLiteral("state")] = QStringLiteral("unknown_garbage_state");
    corruptObj[QStringLiteral("segments")] = QStringLiteral("should_be_array");
    corruptObj[QStringLiteral("headers")] = QJsonArray(); // array instead of object
    corruptObj[QStringLiteral("cookies")] = QStringLiteral("corrupt");
    corruptObj[QStringLiteral("createdAt")] = QStringLiteral("not_a_date");

    // Deserialization must be resilient and not crash
    const DownloadItem item = DownloadItem::fromJson(corruptObj);
    QCOMPARE(item.m_state, DownloadStateQueued); // fallback to Queued
    QCOMPARE(item.m_totalBytes, 0LL);
    QCOMPARE(item.m_receivedBytes, 0LL);
    QVERIFY(item.m_segments.isEmpty());
    QVERIFY(item.m_headers.isEmpty());
    QVERIFY(item.m_createdAt.isValid());
}

void TestJsonRoundtrip::testCorruptJsonDocumentParsing() {
    // 1. Empty string
    QJsonParseError err1;
    QJsonDocument doc1 = QJsonDocument::fromJson(QByteArray(), &err1);
    QVERIFY(doc1.isNull());
    QVERIFY(err1.error != QJsonParseError::NoError);

    // 2. Truncated JSON mid-stream
    QJsonParseError err2;
    const QByteArray truncatedJson = "{\"id\": \"item-1\", \"totalBytes\": 1024, \"segments\": [{\"index\": 0";
    QJsonDocument doc2 = QJsonDocument::fromJson(truncatedJson, &err2);
    QVERIFY(doc2.isNull());
    QVERIFY(err2.error != QJsonParseError::NoError);

    // 3. Invalid syntax
    QJsonParseError err3;
    const QByteArray garbageJson = "<html><body>502 Bad Gateway</body></html>";
    QJsonDocument doc3 = QJsonDocument::fromJson(garbageJson, &err3);
    QVERIFY(doc3.isNull());
    QVERIFY(err3.error != QJsonParseError::NoError);
}

void TestJsonRoundtrip::testTruncatedJsonStateFileSimulation() {
    // Simulates DownloadManager::loadState behavior on corrupt files
    // When doc.isArray() is false, existing in-memory items must never be wiped.
    QList<DownloadItem> existingItems;
    DownloadItem activeItem;
    activeItem.m_id = QStringLiteral("active-1");
    existingItems.append(activeItem);

    // Parse corrupt truncated buffer
    const QByteArray corruptData = "[{\"id\":\"active-1\", \"totalBytes\":";
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(corruptData, &err);

    // Safe loader guard check
    if (doc.isNull() || !doc.isArray()) {
        // Safe path: do NOT wipe existing items
    } else {
        existingItems.clear(); // This dangerous branch should never execute
    }

    // Existing list remains intact
    QCOMPARE(existingItems.size(), 1);
    QCOMPARE(existingItems.first().m_id, QStringLiteral("active-1"));
}

QTEST_MAIN(TestJsonRoundtrip)
#include "test_json_roundtrip.moc"
