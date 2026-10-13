#include <QTest>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QDir>
#include "PathUtils.h"
#include "MediaHosts.h"

#include <QStandardPaths>
#include <QFile>
#include <QChar>
#include "test_guard.h"

class TestPathUtils : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
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
    void testUniqueTargetPathLongFileName();
    void testMediaHostsStreamingSite();
    void testUniqueTargetPathKeepsParenthesizedYear();
    void testSanitizeTruncationIsSurrogateSafe();
    void testUniqueTargetPathTruncationIsSurrogateSafe();
    void testIsInsideDir();

private:
    static QString emojiString(int count);
    static void verifyValidUtf16(const QString &s);};

void TestPathUtils::initTestCase() {
    QString why;
    QVERIFY2(TestGuard::enterSandbox(&why), qPrintable(why));
}

void TestPathUtils::testSanitizeRelativeTraversal() {
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("../../x")), QStringLiteral("x"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("../../../etc/passwd")), QStringLiteral("passwd"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("dir/subdir/file.txt")), QStringLiteral("file.txt"));
}

void TestPathUtils::testSanitizePercentEncodedTraversal() {
    // D7: sanitizeFileName does not decode percent-encoding, preventing double-decoding (e.g. a%2541.zip -> a%2541.zip)
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("a%2541.zip")), QStringLiteral("a%2541.zip"));
    QCOMPARE(PathUtils::sanitizeFileName(QStringLiteral("my%20file.pdf")), QStringLiteral("my%20file.pdf"));
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

    // Verify percent-encoded string stays strictly inside base directory and does not escape
    const QString joinedEnc = PathUtils::safeJoin(base, QStringLiteral("..%2F..%2F..%2Fetc%2Fshadow"));
    QVERIFY(!joinedEnc.isEmpty());
    QCOMPARE(QFileInfo(joinedEnc).absolutePath(), canonicalBase);

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

void TestPathUtils::testUniqueTargetPathLongFileName() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString base = tempDir.path();

    // 300-byte file name (R2 regression test)
    QString longName = QString(296, 'a') + QStringLiteral(".zip");
    QVERIFY(longName.toUtf8().size() >= 300);

    // First resolution
    QString firstPath = PathUtils::uniqueTargetPath(base, longName);
    QVERIFY(!firstPath.isEmpty());
    QVERIFY(!QFile::exists(firstPath));
    QVERIFY(QFileInfo(firstPath).fileName().toUtf8().size() <= 240);

    // Create the first file so the second resolution encounters a collision
    QFile f1(firstPath);
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write("test");
    f1.close();

    // Second resolution: must NOT loop forever, must produce a distinct path
    QString secondPath = PathUtils::uniqueTargetPath(base, longName);
    QVERIFY(!secondPath.isEmpty());
    QVERIFY(secondPath != firstPath);
    QVERIFY(!QFile::exists(secondPath));
    QVERIFY(QFileInfo(secondPath).fileName().toUtf8().size() <= 240);
    QVERIFY(secondPath.contains(QStringLiteral(" (1).zip")));
}

void TestPathUtils::testMediaHostsStreamingSite() {
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("www.dropbox.com")), false);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("m.youtube.com")), true);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("x.com")), true);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("linux.com")), false);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("xbox.com")), false);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("youtu.be")), true);
    QCOMPARE(MediaHosts::isStreamingSite(QStringLiteral("vimeo.com")), true);
}

QString TestPathUtils::emojiString(int count) {
    // U+1F600 is stored as a surrogate pair in UTF-16
    QString s;
    for (int i = 0; i < count; ++i) {
        s.append(QChar::highSurrogate(0x1F600));
        s.append(QChar::lowSurrogate(0x1F600));
    }
    return s;
}

void TestPathUtils::verifyValidUtf16(const QString &s) {
    for (int i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        QVERIFY2(c.unicode() != 0xFFFD, "U+FFFD replacement character found");
        if (c.isHighSurrogate()) {
            QVERIFY2(i + 1 < s.size() && s.at(i + 1).isLowSurrogate(), "lone high surrogate");
            ++i;
        } else {
            QVERIFY2(!c.isLowSurrogate(), "lone low surrogate");
        }
    }
    // A valid string survives a UTF-8 round trip unchanged
    QCOMPARE(QString::fromUtf8(s.toUtf8()), s);
}

void TestPathUtils::testUniqueTargetPathKeepsParenthesizedYear() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString base = tempDir.path();

    // E6: "(2019)" belongs to the name and must not be stripped
    const QString first = PathUtils::uniqueTargetPath(base, QStringLiteral("Movie (2019).mkv"));
    QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("Movie (2019).mkv"));

    QFile f(first);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    const QString second = PathUtils::uniqueTargetPath(base, QStringLiteral("Movie (2019).mkv"));
    QCOMPARE(QFileInfo(second).fileName(), QStringLiteral("Movie (2019) (1).mkv"));
}

void TestPathUtils::testSanitizeTruncationIsSurrogateSafe() {
    // 300 emoji = 1200 UTF-8 bytes: must be cut to <= 240 bytes on a character boundary
    const QString longEmoji = emojiString(300);
    const QString cut = PathUtils::sanitizeFileName(longEmoji);
    QVERIFY(!cut.isEmpty());
    QVERIFY(cut.toUtf8().size() <= 240);
    verifyValidUtf16(cut);

    // The same with an extension that has to survive
    const QString cutExt = PathUtils::sanitizeFileName(longEmoji + QStringLiteral(".zip"));
    QVERIFY(cutExt.toUtf8().size() <= 240);
    QVERIFY(cutExt.endsWith(QStringLiteral(".zip")));
    verifyValidUtf16(cutExt);

    // 239 ASCII bytes + one emoji (4 bytes) = 243: the emoji must go away as a whole
    const QString edge = QString(239, QLatin1Char('a')) + emojiString(1);
    const QString cutEdge = PathUtils::sanitizeFileName(edge);
    QVERIFY(cutEdge.toUtf8().size() <= 240);
    verifyValidUtf16(cutEdge);
    QVERIFY(!cutEdge.contains(QChar::highSurrogate(0x1F600)));
}

void TestPathUtils::testUniqueTargetPathTruncationIsSurrogateSafe() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString base = tempDir.path();

    const QString name = emojiString(300) + QStringLiteral(".bin");
    const QString first = PathUtils::uniqueTargetPath(base, name);
    QVERIFY(QFileInfo(first).fileName().toUtf8().size() <= 240);
    verifyValidUtf16(QFileInfo(first).fileName());

    QFile f(first);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();

    // The " (1)" tag forces another cut; it must still end on a character boundary
    const QString second = PathUtils::uniqueTargetPath(base, name);
    QVERIFY(second != first);
    QVERIFY(QFileInfo(second).fileName().toUtf8().size() <= 240);
    QVERIFY(second.contains(QStringLiteral(" (1).bin")));
    verifyValidUtf16(QFileInfo(second).fileName());
}

void TestPathUtils::testIsInsideDir() {
    QTemporaryDir tempDir;
    QTemporaryDir otherDir;
    QVERIFY(tempDir.isValid());
    QVERIFY(otherDir.isValid());
    const QString dir = tempDir.path();
    const QString other = otherDir.path();

    // directly inside, and in an existing sub folder
    QVERIFY(PathUtils::isInsideDir(dir + "/file.bin", dir));
    QVERIFY(QDir().mkpath(dir + "/sub/deeper"));
    QVERIFY(PathUtils::isInsideDir(dir + "/sub/deeper/file.bin", dir));

    // outside: a sibling temp dir, the parent, an absolute system path
    QVERIFY(!PathUtils::isInsideDir(other + "/file.bin", dir));
    QVERIFY(!PathUtils::isInsideDir(QFileInfo(dir).absolutePath() + "/file.bin", dir));
    QVERIFY(!PathUtils::isInsideDir(QStringLiteral("/etc/passwd"), dir));

    // ".." that climbs out of the folder
    QVERIFY(!PathUtils::isInsideDir(dir + "/sub/../../escape.bin", dir));
    // ".." that stays inside
    QVERIFY(PathUtils::isInsideDir(dir + "/sub/../file.bin", dir));

    // a prefix match is not enough: "<dir>-evil" is not inside "<dir>"
    QVERIFY(QDir().mkpath(dir + "-evil"));
    QVERIFY(!PathUtils::isInsideDir(dir + "-evil/file.bin", dir));
    QDir().rmdir(dir + "-evil");

    // a symlinked FOLDER inside dir that points outside: files below it are outside
    QVERIFY(QFile::link(other, dir + "/lnk"));
    QVERIFY(!PathUtils::isInsideDir(dir + "/lnk/file.bin", dir));

    // a symlinked FILE is judged by where the link lives
    QVERIFY(QFile::link(other + "/target.bin", dir + "/filelink"));
    QVERIFY(PathUtils::isInsideDir(dir + "/filelink", dir));

    // missing folders: false, never a crash
    QVERIFY(!PathUtils::isInsideDir(dir + "/missing/file.bin", dir));
    QVERIFY(!PathUtils::isInsideDir(dir + "/file.bin", dir + "/does-not-exist"));
    QVERIFY(!PathUtils::isInsideDir(QString(), dir));
    QVERIFY(!PathUtils::isInsideDir(dir + "/file.bin", QString()));
}

QTEST_MAIN(TestPathUtils)
#include "test_pathutils.moc"
