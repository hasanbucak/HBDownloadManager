#include <QTest>
#include <QObject>
#include <QTemporaryFile>
#include <fcntl.h>
#include <unistd.h>
#include "SegmentedDownloader.h"

class TestRangeValidation : public QObject {
    Q_OBJECT

private slots:
    void testGoodContentRange();
    void testWrongStartContentRange();
    void testEndExceedsSegmentEnd();
    void testWrongTotalSize();
    void testWildcardTotal();
    void testMalformedHeaders();
    void testWriteFully();
};

void TestRangeValidation::testGoodContentRange() {
    qint64 first = -1;
    qint64 last = -1;
    bool ok = SegmentedDownloader::validateContentRangeHeader(
        QStringLiteral("bytes 0-1023/2048"),
        0,      // expectedStart
        1023,   // segEnd
        2048,   // totalBytes
        &first,
        &last
    );

    QVERIFY(ok);
    QCOMPARE(first, 0);
    QCOMPARE(last, 1023);
}

void TestRangeValidation::testWrongStartContentRange() {
    qint64 first = -1;
    qint64 last = -1;
    bool ok = SegmentedDownloader::validateContentRangeHeader(
        QStringLiteral("bytes 100-1023/2048"),
        0,      // expectedStart was 0, but header says 100
        1023,
        2048,
        &first,
        &last
    );

    QVERIFY(!ok);
}

void TestRangeValidation::testEndExceedsSegmentEnd() {
    qint64 first = -1;
    qint64 last = -1;
    bool ok = SegmentedDownloader::validateContentRangeHeader(
        QStringLiteral("bytes 0-1500/2048"),
        0,
        1023,   // segEnd is 1023, but header says 1500
        2048,
        &first,
        &last
    );

    QVERIFY(!ok);
}

void TestRangeValidation::testWrongTotalSize() {
    qint64 first = -1;
    qint64 last = -1;
    bool ok = SegmentedDownloader::validateContentRangeHeader(
        QStringLiteral("bytes 0-1023/4096"),
        0,
        1023,
        2048,   // known total is 2048, but header says 4096 (file changed size)
        &first,
        &last
    );

    QVERIFY(!ok);
}

void TestRangeValidation::testWildcardTotal() {
    qint64 first = -1;
    qint64 last = -1;
    bool ok = SegmentedDownloader::validateContentRangeHeader(
        QStringLiteral("bytes 0-1023/*"),
        0,
        1023,
        2048,   // total is unknown/wildcard
        &first,
        &last
    );

    QVERIFY(ok);
    QCOMPARE(first, 0);
    QCOMPARE(last, 1023);
}

void TestRangeValidation::testMalformedHeaders() {
    QVERIFY(!SegmentedDownloader::validateContentRangeHeader(QString(), 0, 1023, 2048));
    QVERIFY(!SegmentedDownloader::validateContentRangeHeader(QStringLiteral("invalid"), 0, 1023, 2048));
    QVERIFY(!SegmentedDownloader::validateContentRangeHeader(QStringLiteral("bytes -/"), 0, 1023, 2048));
    QVERIFY(!SegmentedDownloader::validateContentRangeHeader(QStringLiteral("bytes 500-100/1000"), 500, 1000, 1000)); // first > last
    QVERIFY(!SegmentedDownloader::validateContentRangeHeader(QStringLiteral("characters 0-100/1000"), 0, 100, 1000));
}

void TestRangeValidation::testWriteFully() {
    QTemporaryFile tmp;
    QVERIFY(tmp.open());
    int fd = tmp.handle();
    QVERIFY(fd >= 0);

    QByteArray data("Hello IDM-Grade Reliability!");
    int err = 0;
    bool ok = SegmentedDownloader::writeFully(fd, data.constData(), data.size(), 0, &err);
    QVERIFY(ok);
    QCOMPARE(err, 0);

    // Verify offset writing
    QByteArray data2(" Reliable");
    ok = SegmentedDownloader::writeFully(fd, data2.constData(), data2.size(), 5, &err);
    QVERIFY(ok);

    tmp.seek(0);
    QByteArray readBack = tmp.readAll();
    QVERIFY(readBack.startsWith("Hello Reliable"));
}

QTEST_MAIN(TestRangeValidation)
#include "test_range_validation.moc"
