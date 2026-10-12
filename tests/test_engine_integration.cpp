#include <QTest>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QRegularExpression>
#include "SegmentedDownloader.h"
#include "DownloadItem.h"

class TestEngineIntegration : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void testNormal4Segments();
    void testManySegments8();
    void testManySegments16();
    void testIgnoreRange();
    void testDropHalfway();
    void testForbidden();
    void testTraversalHeader();
    void testNoRange();

private:
    QByteArray computeFileSha256(const QString &filePath);
    bool runDownload(const QString &endpoint, int segments, QString &outFinalPath, QString &outError);

    QProcess *m_serverProcess;
    QString m_serverBaseUrl;
    QByteArray m_expectedSha256;
};

void TestEngineIntegration::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);
    QString configDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/hb-download-manager";
    QVERIFY2(!configDir.startsWith(QDir::homePath() + "/.config/"), "Safety check failed: test mode not isolating config path!");

    // Generate expected 128 KiB test payload SHA-256 matching test_server.py
    const int payloadSize = 128 * 1024;
    QByteArray payload(payloadSize, 0);
    for (int i = 0; i < payloadSize; ++i) {
        payload[i] = static_cast<char>((i * 31 + 17) % 256);
    }
    m_expectedSha256 = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();

    // Locate test_server.py
    QStringList searchPaths;
    searchPaths << QStringLiteral("tests/test_server.py")
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
    QVERIFY2(m_serverProcess->waitForStarted(5000), "Failed to start test_server.py");

    // Read READY line to obtain port
    bool ready = false;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && !ready) {
        if (m_serverProcess->waitForReadyRead(500)) {
            QByteArray lines = m_serverProcess->readAllStandardOutput();
            static const QRegularExpression readyRe(QStringLiteral("READY (http://127\\.0\\.0\\.1:\\d+)"));
            QRegularExpressionMatch match = readyRe.match(QString::fromUtf8(lines));
            if (match.hasMatch()) {
                m_serverBaseUrl = match.captured(1);
                ready = true;
                break;
            }
        }
    }
    QVERIFY2(ready, "test_server.py did not report READY URL");
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

QByteArray TestEngineIntegration::computeFileSha256(const QString &filePath) {
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

bool TestEngineIntegration::runDownload(const QString &endpoint, int segments, QString &outFinalPath, QString &outError) {
    QTemporaryDir tempDir;
    if (!tempDir.isValid()) return false;
    tempDir.setAutoRemove(false); // Keep files for verification, delete manually

    DownloadItem item;
    item.m_id = QStringLiteral("int-test-") + QString::number(QDateTime::currentMSecsSinceEpoch());
    item.m_url = QUrl(m_serverBaseUrl + endpoint);
    item.m_fileName = QStringLiteral("download.bin");
    item.m_directory = tempDir.path();
    item.m_requestedSegments = segments;

    SegmentedDownloader downloader(item);
    QSignalSpy spy(&downloader, SIGNAL(atFinished(bool, QString)));

    downloader.start();

    // Wait up to 30 seconds for download to complete
    bool finished = spy.wait(30000);
    if (!finished) {
        downloader.cancel();
        outError = QStringLiteral("Timed out waiting for atFinished signal");
        return false;
    }

    QList<QVariant> args = spy.takeFirst();
    bool success = args.at(0).toBool();
    outError = args.at(1).toString();
    outFinalPath = downloader.item().m_finalPath;

    return success;
}

void TestEngineIntegration::testNormal4Segments() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/normal"), 4, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

void TestEngineIntegration::testManySegments8() {
    // R1 Regression verification: 8 segments
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/many-segments"), 8, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

void TestEngineIntegration::testManySegments16() {
    // R1 Regression verification: 16 segments
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/many-segments"), 16, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

void TestEngineIntegration::testIgnoreRange() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/ignore-range"), 4, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

void TestEngineIntegration::testDropHalfway() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/drop-halfway"), 1, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

void TestEngineIntegration::testForbidden() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/forbidden"), 4, finalPath, error);
    QVERIFY(!ok); // Must fail cleanly
    QVERIFY(!error.isEmpty());
}

void TestEngineIntegration::testTraversalHeader() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/traversal-header?type=quotes"), 1, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    // Verify it didn't write outside the directory
    QVERIFY(!finalPath.contains(QStringLiteral("..")));
    QFile::remove(finalPath);
}

void TestEngineIntegration::testNoRange() {
    QString finalPath;
    QString error;
    bool ok = runDownload(QStringLiteral("/no-range"), 4, finalPath, error);
    QVERIFY2(ok, qPrintable(error));
    QVERIFY(QFile::exists(finalPath));
    QCOMPARE(computeFileSha256(finalPath), m_expectedSha256);
    QFile::remove(finalPath);
}

QTEST_MAIN(TestEngineIntegration)
#include "test_engine_integration.moc"
