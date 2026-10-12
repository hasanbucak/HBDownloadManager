#include <QTest>
#include <QStandardPaths>
#include "RangePlanner.h"

class TestRangePlanner : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void testOneByteRanges();
    void test511KiBRanges();
    void test512KiBRanges();
    void testFourGiBPlusOneRanges();
    void testAdaptivePlanning();
    void testContentRangeValidation();

private:
    void verifyExactCoverage(qint64 totalBytes, int requestedSegments);
};

void TestRangePlanner::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
}

void TestRangePlanner::verifyExactCoverage(qint64 totalBytes, int requestedSegments) {
    const QList<SegmentInfo> segments = RangePlanner::planRanges(totalBytes, requestedSegments);

    QVERIFY(!segments.isEmpty());
    QVERIFY(RangePlanner::verifyCoverage(segments, totalBytes));

    // First segment must start at byte 0
    QCOMPARE(segments.first().m_start, 0LL);
    // Last segment must end at byte totalBytes - 1
    QCOMPARE(segments.last().m_end, totalBytes - 1);

    qint64 totalLength = 0;
    for (int i = 0; i < segments.size(); ++i) {
        const SegmentInfo &s = segments.at(i);
        QCOMPARE(s.m_index, i);
        QVERIFY(s.m_start <= s.m_end);
        QVERIFY(s.length() > 0);

        if (i > 0) {
            // Must have strictly no gap and no overlap with preceding segment
            QCOMPARE(s.m_start, segments.at(i - 1).m_end + 1);
        }
        totalLength += s.length();
    }

    // Total byte count must match exactly
    QCOMPARE(totalLength, totalBytes);
}

void TestRangePlanner::testOneByteRanges() {
    const qint64 total = 1LL;

    // 1 segment
    verifyExactCoverage(total, 1);
    QCOMPARE(RangePlanner::planRanges(total, 1).size(), 1);

    // 8 segments (must be clamped to 1 because 1 byte cannot be split into 8)
    verifyExactCoverage(total, 8);
    QCOMPARE(RangePlanner::planRanges(total, 8).size(), 1);

    // 16 segments (must be clamped to 1)
    verifyExactCoverage(total, 16);
    QCOMPARE(RangePlanner::planRanges(total, 16).size(), 1);
}

void TestRangePlanner::test511KiBRanges() {
    const qint64 total = 511LL * 1024LL; // 523,264 bytes

    // 1 segment
    verifyExactCoverage(total, 1);
    QCOMPARE(RangePlanner::planRanges(total, 1).size(), 1);

    // 8 segments
    verifyExactCoverage(total, 8);
    QCOMPARE(RangePlanner::planRanges(total, 8).size(), 8);

    // 16 segments
    verifyExactCoverage(total, 16);
    QCOMPARE(RangePlanner::planRanges(total, 16).size(), 16);
}

void TestRangePlanner::test512KiBRanges() {
    const qint64 total = 512LL * 1024LL; // 524,288 bytes

    // 1 segment
    verifyExactCoverage(total, 1);
    QCOMPARE(RangePlanner::planRanges(total, 1).size(), 1);

    // 8 segments
    verifyExactCoverage(total, 8);
    QCOMPARE(RangePlanner::planRanges(total, 8).size(), 8);

    // 16 segments
    verifyExactCoverage(total, 16);
    QCOMPARE(RangePlanner::planRanges(total, 16).size(), 16);
}

void TestRangePlanner::testFourGiBPlusOneRanges() {
    const qint64 fourGiB = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    const qint64 total = fourGiB + 1LL; // 4,294,967,297 bytes (exceeds 32-bit uint)

    // 1 segment
    verifyExactCoverage(total, 1);
    QCOMPARE(RangePlanner::planRanges(total, 1).size(), 1);

    // 8 segments
    verifyExactCoverage(total, 8);
    QList<SegmentInfo> segs8 = RangePlanner::planRanges(total, 8);
    QCOMPARE(segs8.size(), 8);
    // Verify last segment extends past 32-bit boundary (> 0xFFFFFFFF)
    QVERIFY(segs8.last().m_end > 4294967295LL);

    // 16 segments
    verifyExactCoverage(total, 16);
    QList<SegmentInfo> segs16 = RangePlanner::planRanges(total, 16);
    QCOMPARE(segs16.size(), 16);
    QVERIFY(segs16.last().m_end > 4294967295LL);
}

void TestRangePlanner::testAdaptivePlanning() {
    const qint64 minSegmentSize = 512LL * 1024LL;

    // 1 byte with 8 target segments -> 1 segment
    QList<SegmentInfo> segs1 = RangePlanner::planAdaptive(1LL, 8, minSegmentSize, true);
    QCOMPARE(segs1.size(), 1);
    QVERIFY(RangePlanner::verifyCoverage(segs1, 1LL));

    // 511 KiB with 8 target segments -> 1 segment (below minSegmentSize)
    QList<SegmentInfo> segs511 = RangePlanner::planAdaptive(511LL * 1024LL, 8, minSegmentSize, true);
    QCOMPARE(segs511.size(), 1);
    QVERIFY(RangePlanner::verifyCoverage(segs511, 511LL * 1024LL));

    // 512 KiB with 8 target segments -> 1 segment (512 KiB / 512 KiB = 1)
    QList<SegmentInfo> segs512 = RangePlanner::planAdaptive(512LL * 1024LL, 8, minSegmentSize, true);
    QCOMPARE(segs512.size(), 1);
    QVERIFY(RangePlanner::verifyCoverage(segs512, 512LL * 1024LL));

    // 8 MiB with 8 target segments -> 8 segments
    QList<SegmentInfo> segs8M = RangePlanner::planAdaptive(8LL * 1024LL * 1024LL, 8, minSegmentSize, true);
    QCOMPARE(segs8M.size(), 8);
    QVERIFY(RangePlanner::verifyCoverage(segs8M, 8LL * 1024LL * 1024LL));

    // When range support is disabled -> always 1 segment
    QList<SegmentInfo> noRanges = RangePlanner::planAdaptive(100LL * 1024LL * 1024LL, 8, minSegmentSize, false);
    QCOMPARE(noRanges.size(), 1);
    QVERIFY(RangePlanner::verifyCoverage(noRanges, 100LL * 1024LL * 1024LL));
}

void TestRangePlanner::testContentRangeValidation() {
    // Valid 206 Partial Content range
    QVERIFY(RangePlanner::validateContentRange(206, QStringLiteral("bytes 0-499/1000"), 0, 499, 1000));
    QVERIFY(RangePlanner::validateContentRange(206, QStringLiteral("bytes 500-999/1000"), 500, 999, 1000));

    // Case-insensitive verification (BYTES)
    QVERIFY(RangePlanner::validateContentRange(206, QStringLiteral("BYTES 0-499/1000"), 0, 499, 1000));
    QVERIFY(RangePlanner::validateContentRange(206, QStringLiteral("Bytes 500-999/1000"), 500, 999, 1000));

    // Valid with wildcard remote size
    QVERIFY(RangePlanner::validateContentRange(206, QStringLiteral("bytes 0-499/*"), 0, 499, 1000));

    // Rejection: HTTP 200 (server ignored Range header)
    QVERIFY(!RangePlanner::validateContentRange(200, QStringLiteral("bytes 0-499/1000"), 0, 499, 1000));

    // Rejection: HTTP 403 Forbidden
    QVERIFY(!RangePlanner::validateContentRange(403, QStringLiteral(""), 0, 499, 1000));

    // Rejection: Mismatched start offset
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("bytes 100-499/1000"), 0, 499, 1000));

    // Rejection: Exceeds segment end boundary
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("bytes 0-600/1000"), 0, 499, 1000));

    // Rejection: Remote file size mismatch (changed size)
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("bytes 0-499/2000"), 0, 499, 1000));

    // Rejection: Inverted range
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("bytes 500-200/1000"), 500, 200, 1000));

    // Rejection: Malformed headers
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral(""), 0, 499, 1000));
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("bytes none"), 0, 499, 1000));
    QVERIFY(!RangePlanner::validateContentRange(206, QStringLiteral("invalid"), 0, 499, 1000));
}

QTEST_MAIN(TestRangePlanner)
#include "test_rangeplanner.moc"
