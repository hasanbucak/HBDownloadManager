#include <QTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include "PathUtils.h"

class TestPathUtils : public QObject {
    Q_OBJECT

private slots:
    void testSanitizeRelativeTraversal();
    void testSanitizePercentEncodedTraversal();
    void testSanitizeAbsolutePath();
    void testSanitizeWindowsBackslash();
    void testSanitizeEmpty();
    void testSanitizeLongFileName();
    void testSanitizeControlChars();
    void testSafeJoinValid();
    void testSafeJoinRejectsEscape();
    void testSafeJoinInvalidDir();
};

void TestPathUtils::testSanitizeRelativeTraversal() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("../../x")), QStringLiteral("x"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("../../../etc/passwd")), QStringLiteral("passwd"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("dir/subdir/file.txt")), QStringLiteral("file.txt"));
}

void TestPathUtils::testSanitizePercentEncodedTraversal() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("..%2F..%2Fx")), QStringLiteral("x"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("..%2f..%2fescape.tar.gz")), QStringLiteral("escape.tar.gz"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("%2e%2e%2f%2e%2e%2fmalicious.sh")), QStringLiteral("malicious.sh"));
}

void TestPathUtils::testSanitizeAbsolutePath() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("/etc/x")), QStringLiteral("x"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("/var/log/syslog")), QStringLiteral("syslog"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("/root/.bashrc")), QStringLiteral("bashrc"));
}

void TestPathUtils::testSanitizeWindowsBackslash() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("a\\b")), QStringLiteral("b"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("C:\\Windows\\System32\\cmd.exe")), QStringLiteral("cmd.exe"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("..\\..\\secret.key")), QStringLiteral("secret.key"));
}

void TestPathUtils::testSanitizeEmpty() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("")), QStringLiteral("download"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("   ")), QStringLiteral("download"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral(".")), QStringLiteral("download"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("..")), QStringLiteral("download"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral(".....")), QStringLiteral("download"));
}

void TestPathUtils::testSanitizeLongFileName() {
    // 300 characters without extension
    const QString long300(300, QLatin1Char('a'));
    const QString sanitizedNoExt = PathUtils::sanitizeFileName(long300);
    QVERIFY(sanitizedNoExt.toUtf8().size() <= 240);
    QVERIFY(!sanitizedNoExt.isEmpty());

    // 300 characters with extension
    const QString longWithExt = QString(300, QLatin1Char('b')) + QStringLiteral(".zip");
    const QString sanitizedWithExt = PathUtils::sanitizeFileName(longWithExt);
    QVERIFY(sanitizedWithExt.toUtf8().size() <= 240);
    QVERIFY(sanitizedWithExt.endsWith(QStringLiteral(".zip")));
}

void TestPathUtils::testSanitizeControlChars() {
    const QString raw = QStringLiteral("hello\x00\x01\x1f\tworld\r\n\x7ftest.bin");
    const QString sanitized = PathUtils::sanitizeFileName(raw);

    // Verify all control characters (< 0x20 or == 0x7F) were replaced
    for (int i = 0; i < sanitized.size(); ++i) {
        const QChar c = sanitized.at(i);
        QVERIFY(c.unicode() >= 0x20);
        QVERIFY(c.unicode() != 0x7f);
    }
    QVERIFY(sanitized.contains(QLatin1Char('_')));
    QVERIFY(!sanitized.contains(QLatin1Char('\r')));
    QVERIFY(!sanitized.contains(QLatin1Char('\n')));
    QVERIFY(!sanitized.contains(QLatin1Char('\t')));
}

void TestPathUtils::testSafeJoinValid() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString base = tempDir.path();
    const QString canonicalBase = QDir(base).canonicalPath();

    const QString joined = PathUtils::safeJoin(base, QStringLiteral("report.pdf"));
    QVERIFY(!joined.isEmpty());
    QCOMPARE(QFileInfo(joined).absolutePath(), canonicalBase);
    QCOMPARE(QFileInfo(joined).fileName(), QStringLiteral("report.pdf"));
}

void TestPathUtils::testSafeJoinRejectsEscape() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString base = tempDir.path();
    const QString canonicalBase = QDir(base).canonicalPath();

    // Verify relative traversal does NOT escape directory
    const QString joinedRel = PathUtils::safeJoin(base, QStringLiteral("../../etc/passwd"));
    QVERIFY(!joinedRel.isEmpty());
    // Result MUST stay strictly inside base directory
    QCOMPARE(QFileInfo(joinedRel).absolutePath(), canonicalBase);
    QCOMPARE(QFileInfo(joinedRel).fileName(), QStringLiteral("passwd"));

    // Verify percent-encoded traversal does NOT escape directory
    const QString joinedEnc = PathUtils::safeJoin(base, QStringLiteral("..%2F..%2F..%2Fetc%2Fshadow"));
    QVERIFY(!joinedEnc.isEmpty());
    QCOMPARE(QFileInfo(joinedEnc).absolutePath(), canonicalBase);
    QCOMPARE(QFileInfo(joinedEnc).fileName(), QStringLiteral("shadow"));

    // Verify absolute path does NOT escape directory
    const QString joinedAbs = PathUtils::safeJoin(base, QStringLiteral("/usr/bin/bash"));
    QVERIFY(!joinedAbs.isEmpty());
    QCOMPARE(QFileInfo(joinedAbs).absolutePath(), canonicalBase);
    QCOMPARE(QFileInfo(joinedAbs).fileName(), QStringLiteral("bash"));
}

void TestPathUtils::testSafeJoinInvalidDir() {
    // Non-existent directory must return empty string (reject)
    const QString resNonExistent = PathUtils::safeJoin(QStringLiteral("/path/does_not_exist_123456789"), QStringLiteral("file.txt"));
    QVERIFY(resNonExistent.isEmpty());

    // Empty directory must return empty string (reject)
    const QString resEmpty = PathUtils::safeJoin(QString(), QStringLiteral("file.txt"));
    QVERIFY(resEmpty.isEmpty());
}

QTEST_MAIN(TestPathUtils)
#include "test_pathutils.moc"
