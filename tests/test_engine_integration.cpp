#include <QTest>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QScopedPointer>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include "SegmentedDownloader.h"
#include "DownloadItem.h"
#include "PathUtils.h"
#include "test_guard.h"

namespace {

const int kPayloadSize = 16 * 1024 * 1024;   // must match tests/test_server.py
const int kShrinkSize = 4 * 1024 * 1024;
const int kPeriod = 251;

// Same pattern as test_server.py: bytes(range(251)) repeated
QByteArray makePayload(int size) {
    QByteArray p(size, 0);
    for (int i = 0; i < size; ++i) {
        p[i] = static_cast<char>(i % kPeriod);
    }
    return p;
}

QByteArray sha256Hex(const QByteArray &data) {
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

// Short timeouts for the stall test; the defaults are restored on scope exit
struct ShortTimeouts {
    ShortTimeouts() { SegmentedDownloader::setTimeoutsForTesting(2000, 4000, 3000); }
    ~ShortTimeouts() { SegmentedDownloader::setTimeoutsForTesting(30000, 60000, 15000); }
};

struct RunResult {
    RunResult() : m_ok(false), m_timedOut(false) {}
    bool m_ok;
    bool m_timedOut;
    QString m_error;
    QString m_finalPath;
    DownloadItem m_item;
};

} // namespace

class TestEngineIntegration : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void testNormal4Segments();
    void testStrongEtagSendsIfRange();
    void testManySegments8();
    void testManySegments16QueuesAboveCap();
    void testIgnoreRange();
    void testDropHalfway();
    void testForbidden();
    void testTraversalHeader();
    void testNoRange();
    void testStallRecovers();
    void testChangingFileRestartsCleanly();
    void testNoRangePauseResume();
    void testPauseResumeKeepsEtag();
    void testFlakyFallbackDoesNotCrash();
    void testResumeWithAllSegmentsDone();
    void testWeakEtagNeverSendsIfRange();
    void testSingleStreamAdoptsContentLength();

private:
    QByteArray fileSha256(const QString &filePath);
    QByteArray httpGet(const QString &path);
    qint64 serverStat(const QString &path, const QString &key);
    void resetServerStats();
    bool waitForSignal(QSignalSpy &spy, int ms);
    bool waitForReceived(const SegmentedDownloader &d, qint64 bytes, int ms);
    DownloadItem makeItem(const QString &endpoint, int segments, const QString &fileName = QStringLiteral("download.bin"));
    bool runDownload(const QString &endpoint, int segments, RunResult &out,
                     const QString &fileName = QStringLiteral("download.bin"), int timeoutMs = 120000);

    QProcess *m_serverProcess;
    QString m_serverBaseUrl;
    QByteArray m_payload;
    QByteArray m_expectedSha;
    QByteArray m_changedSha;
    QByteArray m_shrinkSha;
    QScopedPointer<QTemporaryDir> m_dir;   // auto-removing, one per test
    int m_counter;
};

void TestEngineIntegration::initTestCase() {
    QString why;
    QVERIFY2(TestGuard::enterSandbox(&why), qPrintable(why));

    m_serverProcess = NULL;
    m_counter = 0;

    m_payload = makePayload(kPayloadSize);
    m_expectedSha = sha256Hex(m_payload);
    QByteArray changed(m_payload);
    for (int i = 0; i < changed.size(); ++i) {
        changed[i] = static_cast<char>(~static_cast<unsigned char>(changed[i]));
    }
    m_changedSha = sha256Hex(changed);
    m_shrinkSha = sha256Hex(m_payload.left(kShrinkSize));

    // Locate test_server.py
    QStringList searchPaths;
    searchPaths << QStringLiteral("tests/test_server.py")
                << QStringLiteral("test_server.py")
                << QStringLiteral("../tests/test_server.py")
                << QStringLiteral("../../tests/test_server.py")
                << QDir(QCoreApplication::applicationDirPath()).filePath("test_server.py")
                << QDir(QCoreApplication::applicationDirPath()).filePath("../tests/test_server.py")
                << QDir(QCoreApplication::applicationDirPath()).filePath("../../tests/test_server.py")
                << QDir(QCoreApplication::applicationDirPath()).filePath("../../../tests/test_server.py");

    QString serverScript;
    for (int i = 0; i < searchPaths.size(); ++i) {
        if (QFile::exists(searchPaths.at(i))) {
            serverScript = QFileInfo(searchPaths.at(i)).canonicalFilePath();
            break;
        }
    }
    QVERIFY2(!serverScript.isEmpty(), "Could not find test_server.py");

    m_serverProcess = new QProcess(this);
    m_serverProcess->setProgram(QStringLiteral("python3"));
    m_serverProcess->setArguments(QStringList() << serverScript << QStringLiteral("--port") << QStringLiteral("0"));
    m_serverProcess->start();
    QVERIFY2(m_serverProcess->waitForStarted(5000), "Failed to start test_server.py (is python3 installed?)");

    // The first stdout line is "READY http://127.0.0.1:<port>"; the payload hashing takes a moment
    bool ready = false;
    QByteArray collected;
    QElapsedTimer timer;
    timer.start();
    static const QRegularExpression readyRe(QStringLiteral("READY (http://127\\.0\\.0\\.1:\\d+)"));
    while (timer.elapsed() < 20000 && !ready) {
        if (m_serverProcess->waitForReadyRead(500)) {
            collected.append(m_serverProcess->readAllStandardOutput());
            QRegularExpressionMatch match = readyRe.match(QString::fromUtf8(collected));
            if (match.hasMatch()) {
                m_serverBaseUrl = match.captured(1);
                ready = true;
            }
        }
        if (m_serverProcess->state() == QProcess::NotRunning) {
            break;
        }
    }
    QVERIFY2(ready, "test_server.py did not report a READY URL");
}

void TestEngineIntegration::cleanupTestCase() {
    if (m_serverProcess != NULL) {
        m_serverProcess->terminate();
        if (!m_serverProcess->waitForFinished(2000)) {
            m_serverProcess->kill();
            m_serverProcess->waitForFinished(1000);
        }
        delete m_serverProcess;
        m_serverProcess = NULL;
    }
}

void TestEngineIntegration::init() {
    m_dir.reset(new QTemporaryDir());   // auto-remove is the default
    QVERIFY(m_dir->isValid());
    resetServerStats();
}

void TestEngineIntegration::cleanup() {
    SegmentedDownloader::setTimeoutsForTesting(30000, 60000, 15000);
    m_dir.reset();
}

QByteArray TestEngineIntegration::fileSha256(const QString &filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (hash.addData(&file)) {
        return hash.result().toHex();
    }
    return QByteArray();
}

QByteArray TestEngineIntegration::httpGet(const QString &path) {
    QNetworkAccessManager nam;
    QNetworkReply *reply = nam.get(QNetworkRequest(QUrl(m_serverBaseUrl + path)));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray data = reply->isFinished() ? reply->readAll() : QByteArray();
    reply->abort();
    reply->deleteLater();
    return data;
}

qint64 TestEngineIntegration::serverStat(const QString &path, const QString &key) {
    const QJsonDocument doc = QJsonDocument::fromJson(httpGet(QStringLiteral("/stats")));
    const QJsonObject paths = doc.object().value(QStringLiteral("paths")).toObject();
    const QJsonObject entry = paths.value(path).toObject();
    return static_cast<qint64>(entry.value(key).toDouble(-1));
}

void TestEngineIntegration::resetServerStats() {
    httpGet(QStringLiteral("/reset"));
}

bool TestEngineIntegration::waitForSignal(QSignalSpy &spy, int ms) {
    // The signal may already have been emitted synchronously (e.g. from start())
    if (spy.count() > 0) {
        return true;
    }
    return spy.wait(ms);
}

bool TestEngineIntegration::waitForReceived(const SegmentedDownloader &d, qint64 bytes, int ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < ms) {
        if (d.item().m_receivedBytes >= bytes) {
            return true;
        }
        QTest::qWait(25);
    }
    return d.item().m_receivedBytes >= bytes;
}

DownloadItem TestEngineIntegration::makeItem(const QString &endpoint, int segments, const QString &fileName) {
    DownloadItem item;
    item.m_id = QStringLiteral("int-test-%1-%2").arg(QDateTime::currentMSecsSinceEpoch()).arg(++m_counter);
    item.m_url = QUrl(m_serverBaseUrl + endpoint);
    item.m_fileName = fileName;
    item.m_directory = m_dir->path();
    item.m_requestedSegments = segments;
    return item;
}

bool TestEngineIntegration::runDownload(const QString &endpoint, int segments, RunResult &out,
                                        const QString &fileName, int timeoutMs) {
    SegmentedDownloader downloader(makeItem(endpoint, segments, fileName));
    QSignalSpy spy(&downloader, &SegmentedDownloader::atFinished);

    downloader.start();

    if (!waitForSignal(spy, timeoutMs)) {
        downloader.cancel();
        out.m_timedOut = true;
        out.m_error = QStringLiteral("Timed out waiting for atFinished");
        out.m_item = downloader.item();
        return false;
    }

    const QList<QVariant> args = spy.takeFirst();
    out.m_ok = args.at(0).toBool();
    out.m_error = args.at(1).toString();
    out.m_item = downloader.item();
    out.m_finalPath = out.m_item.m_finalPath;
    return out.m_ok;
}

void TestEngineIntegration::testNormal4Segments() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/normal"), 4, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_segments.size(), 4);   // T1: the payload is big enough for 4 real segments
    QVERIFY(PathUtils::isInsideDir(r.m_finalPath, m_dir->path()));
    QCOMPARE(QFileInfo(r.m_finalPath).size(), static_cast<qint64>(kPayloadSize));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testStrongEtagSendsIfRange() {
    // Positive control for the weak-ETag test: a strong validator IS sent as If-Range on segments
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/normal"), 4, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_etag, QStringLiteral("\"normal-v1\""));
    QVERIFY2(serverStat(QStringLiteral("/normal"), QStringLiteral("if_range")) > 0,
             "segment requests must carry If-Range for a strong ETag");
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testManySegments8() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/many-segments"), 8, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_segments.size(), 8);
    QVERIFY(PathUtils::isInsideDir(r.m_finalPath, m_dir->path()));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testManySegments16QueuesAboveCap() {
    // R1 / E1 regression: 16 segments, only 6 replies may run at the same time
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/many-segments"), 16, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_segments.size(), 16);
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);

    const qint64 maxConcurrent = serverStat(QStringLiteral("/many-segments"), QStringLiteral("max_concurrent"));
    QVERIFY2(maxConcurrent <= SegmentedDownloader::s_maxParallelReplies,
             qPrintable(QStringLiteral("server saw %1 simultaneous segment requests").arg(maxConcurrent)));
    QVERIFY2(maxConcurrent >= 2, "segments must run in parallel");
}

void TestEngineIntegration::testIgnoreRange() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/ignore-range"), 4, r), qPrintable(r.m_error));
    QVERIFY(PathUtils::isInsideDir(r.m_finalPath, m_dir->path()));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testDropHalfway() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/drop-halfway"), 1, r), qPrintable(r.m_error));
    QCOMPARE(serverStat(QStringLiteral("/drop-halfway"), QStringLiteral("dropped")), static_cast<qint64>(1));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testForbidden() {
    RunResult r;
    QVERIFY(!runDownload(QStringLiteral("/forbidden"), 4, r));   // must fail cleanly, not time out
    QVERIFY(!r.m_timedOut);
    QVERIFY(!r.m_error.isEmpty());
}

void TestEngineIntegration::testTraversalHeader() {
    // File name "download" lets the Content-Disposition name win; it must be reduced to a plain name
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/traversal-header?type=quotes"), 1, r, QStringLiteral("download")),
             qPrintable(r.m_error));
    QVERIFY(QFile::exists(r.m_finalPath));
    QVERIFY(PathUtils::isInsideDir(r.m_finalPath, m_dir->path()));
    QCOMPARE(QFileInfo(r.m_finalPath).absolutePath(), QDir(m_dir->path()).canonicalPath());
    QCOMPARE(QFileInfo(r.m_finalPath).fileName(), QStringLiteral("evil_shell.sh"));
    QVERIFY(!r.m_finalPath.contains(QStringLiteral("..")));
}

void TestEngineIntegration::testNoRange() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/no-range"), 4, r), qPrintable(r.m_error));
    QVERIFY(r.m_item.m_segments.isEmpty());
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testStallRecovers() {
    // T2: the first segment request sends half its body and hangs; the stall watchdog must abort
    // and retry it. Short timeouts (2 s stall) keep the test fast.
    ShortTimeouts shortTimeouts;
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/stall?stalls=1"), 4, r, QStringLiteral("download.bin"), 90000),
             qPrintable(r.m_error));
    QCOMPARE(serverStat(QStringLiteral("/stall"), QStringLiteral("stalled")), static_cast<qint64>(1));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testChangingFileRestartsCleanly() {
    // E3/E4: the file changes right after the probe. The If-Range segment requests get 200 with a
    // new ETag, so the engine must restart from a fresh probe and end with the NEW content only.
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/changing"), 4, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_etag, QStringLiteral("\"etag-v2\""));
    QCOMPARE(QFileInfo(r.m_finalPath).size(), static_cast<qint64>(kPayloadSize));
    QCOMPARE(fileSha256(r.m_finalPath), m_changedSha);   // no splice of old and new bytes
}

void TestEngineIntegration::testNoRangePauseResume() {
    // A server without ranges: pause, build a new downloader from durableItem(), start again
    DownloadItem saved;
    {
        QScopedPointer<SegmentedDownloader> d1(new SegmentedDownloader(makeItem(QStringLiteral("/no-range"), 4)));
        d1->start();
        QVERIFY2(waitForReceived(*d1, 1024 * 1024, 60000), "no data arrived before the pause");
        d1->pause();
        QCOMPARE(d1->item().m_state, DownloadStatePaused);
        saved = d1->durableItem();
        QVERIFY(saved.m_receivedBytes > 0);
        QVERIFY(QFile::exists(saved.partFilePath()));
    }

    SegmentedDownloader d2(saved);
    QSignalSpy spy(&d2, &SegmentedDownloader::atFinished);
    d2.start();
    QVERIFY2(waitForSignal(spy, 120000), "resumed download did not finish");
    const QList<QVariant> args = spy.takeFirst();
    QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
    const QString finalPath = d2.item().m_finalPath;
    QVERIFY(PathUtils::isInsideDir(finalPath, m_dir->path()));
    QCOMPARE(fileSha256(finalPath), m_expectedSha);
}

void TestEngineIntegration::testPauseResumeKeepsEtag() {
    // E3: the durable item of a paused segmented download keeps the (strong) ETag, so the resume
    // sends If-Range and finishes with the right bytes.
    DownloadItem saved;
    {
        QScopedPointer<SegmentedDownloader> d1(new SegmentedDownloader(makeItem(QStringLiteral("/many-segments"), 4)));
        d1->start();
        QVERIFY2(waitForReceived(*d1, 1024 * 1024, 60000), "no data arrived before the pause");
        d1->pause();
        saved = d1->durableItem();
    }
    QCOMPARE(saved.m_segments.size(), 4);
    QCOMPARE(saved.m_etag, QStringLiteral("\"slow-v1\""));
    QVERIFY(saved.m_receivedBytes > 0);
    QVERIFY(saved.m_receivedBytes < kPayloadSize);

    resetServerStats();
    SegmentedDownloader d2(saved);
    QSignalSpy spy(&d2, &SegmentedDownloader::atFinished);
    d2.start();
    QVERIFY2(waitForSignal(spy, 120000), "resumed download did not finish");
    const QList<QVariant> args = spy.takeFirst();
    QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
    QVERIFY2(serverStat(QStringLiteral("/many-segments"), QStringLiteral("if_range")) > 0,
             "the resume must send If-Range (no probe was needed)");
    QCOMPARE(fileSha256(d2.item().m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testFlakyFallbackDoesNotCrash() {
    // E1: one segment request is dropped before any header (a retry gets queued), the other
    // segments get "200 + full body", so the engine falls back to a single stream. The queued
    // retry must then do nothing (no out-of-bounds access, no second writer on the part file).
    SegmentedDownloader downloader(makeItem(QStringLiteral("/flaky"), 4));
    QSignalSpy spy(&downloader, &SegmentedDownloader::atFinished);
    downloader.start();
    QVERIFY2(waitForSignal(spy, 120000), "flaky download did not finish");
    const QList<QVariant> args = spy.takeFirst();
    QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));
    const QString finalPath = downloader.item().m_finalPath;
    QCOMPARE(serverStat(QStringLiteral("/flaky"), QStringLiteral("dropped")), static_cast<qint64>(1));
    QCOMPARE(fileSha256(finalPath), m_expectedSha);

    // Let any leftover retry timer (1-5 s) fire; the downloader must stay quiet and alive
    QTest::qWait(3000);
    QCOMPARE(spy.count(), 0);
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(fileSha256(finalPath), m_expectedSha);
}

void TestEngineIntegration::testResumeWithAllSegmentsDone() {
    // E2: every segment is done and the .part has its full size. The resume must complete at once,
    // without any network access (the URL is unreachable on purpose).
    const qint64 total = 2 * 1024 * 1024;
    const QByteArray data = m_payload.left(static_cast<int>(total));

    DownloadItem item;
    item.m_id = QStringLiteral("e2-all-done-%1").arg(++m_counter);
    item.m_url = QUrl(QStringLiteral("http://127.0.0.1:9/x"));
    item.m_fileName = QStringLiteral("offline.bin");
    item.m_directory = m_dir->path();
    item.m_totalBytes = total;
    item.m_receivedBytes = total;
    item.m_supportsRanges = true;
    item.m_requestedSegments = 2;
    item.m_segments.append(SegmentInfo(0, 0, total / 2 - 1, total / 2));
    item.m_segments.append(SegmentInfo(1, total / 2, total - 1, total / 2));

    {
        QFile part(item.partFilePath());
        QVERIFY(part.open(QIODevice::WriteOnly));
        QCOMPARE(part.write(data), static_cast<qint64>(data.size()));
        part.close();
    }
    QCOMPARE(QFileInfo(item.partFilePath()).size(), total);

    SegmentedDownloader downloader(item);
    QSignalSpy spy(&downloader, &SegmentedDownloader::atFinished);
    downloader.start();
    QVERIFY2(waitForSignal(spy, 10000), "a resume with every segment done must finish immediately");
    const QList<QVariant> args = spy.takeFirst();
    QVERIFY2(args.at(0).toBool(), qPrintable(args.at(1).toString()));

    const QString finalPath = downloader.item().m_finalPath;
    QVERIFY(PathUtils::isInsideDir(finalPath, m_dir->path()));
    QCOMPARE(fileSha256(finalPath), sha256Hex(data));
    QVERIFY(!QFile::exists(item.partFilePath()));
}

void TestEngineIntegration::testWeakEtagNeverSendsIfRange() {
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/weak-etag"), 4, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_segments.size(), 4);
    QVERIFY2(r.m_item.m_etag.isEmpty(), "a weak ETag must not be kept");
    QCOMPARE(serverStat(QStringLiteral("/weak-etag"), QStringLiteral("if_range")), static_cast<qint64>(0));
    QCOMPARE(fileSha256(r.m_finalPath), m_expectedSha);
}

void TestEngineIntegration::testSingleStreamAdoptsContentLength() {
    // E5: the probe says 16 MiB, the real GET only has 4 MiB. The single stream must adopt the new
    // Content-Length and finish instead of retrying until it errors out.
    RunResult r;
    QVERIFY2(runDownload(QStringLiteral("/shrinking"), 4, r), qPrintable(r.m_error));
    QCOMPARE(r.m_item.m_totalBytes, static_cast<qint64>(kShrinkSize));
    QCOMPARE(QFileInfo(r.m_finalPath).size(), static_cast<qint64>(kShrinkSize));
    QCOMPARE(fileSha256(r.m_finalPath), m_shrinkSha);
}

QTEST_MAIN(TestEngineIntegration)
#include "test_engine_integration.moc"
