#pragma once

// Shared safety net for every test: nothing may read or write the developer's real configuration
// (downloads.json, bridge-token, QSettings). Call TestGuard::enterSandbox() first thing in
// initTestCase(), BEFORE touching DownloadManager, LocalServer, BridgeToken or QSettings.
//
//   QString why;
//   QVERIFY2(TestGuard::enterSandbox(&why), qPrintable(why));

#include <QString>
#include <QDir>
#include <QTemporaryDir>
#include <QStandardPaths>
#include <QSettings>
#include <QByteArray>

namespace TestGuard {

inline QTemporaryDir &sandboxDir() {
    static QTemporaryDir dir;   // removed automatically at process exit
    return dir;
}

// The home directory the process was started with (captured before the sandbox replaces HOME)
inline QString &realHomeRef() {
    static QString home;
    return home;
}

inline bool enterSandbox(QString *why = NULL) {
    QString dummy;
    QString &msg = (why != NULL) ? *why : dummy;

    if (!sandboxDir().isValid()) {
        msg = QStringLiteral("could not create the sandbox temp dir");
        return false;
    }
    if (realHomeRef().isEmpty()) {
        realHomeRef() = QDir::homePath();
    }
    const QString root = QDir(sandboxDir().path()).canonicalPath();

    // 1. environment first: QStandardPaths and QSettings read it lazily
    qputenv("HOME", root.toLocal8Bit());
    qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/xdg-config")).toLocal8Bit());
    qputenv("XDG_DATA_HOME", (root + QStringLiteral("/xdg-data")).toLocal8Bit());
    qputenv("XDG_CACHE_HOME", (root + QStringLiteral("/xdg-cache")).toLocal8Bit());
    QDir().mkpath(root + QStringLiteral("/xdg-config"));
    QDir().mkpath(root + QStringLiteral("/xdg-data"));
    QDir().mkpath(root + QStringLiteral("/xdg-cache"));

    // 2. QSettings (native and ini, user and system scope) into the sandbox
    const QString settingsRoot = root + QStringLiteral("/qsettings");
    QDir().mkpath(settingsRoot);
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, settingsRoot);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsRoot);
    QSettings::setPath(QSettings::NativeFormat, QSettings::SystemScope, settingsRoot);
    QSettings::setPath(QSettings::IniFormat, QSettings::SystemScope, settingsRoot);

    // 3. only now switch QStandardPaths to test mode
    QStandardPaths::setTestModeEnabled(true);

    // 4. prove it
    const QString cfg = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    const QString realCfg = realHomeRef() + QStringLiteral("/.config");
    if (cfg.isEmpty()) {
        msg = QStringLiteral("config location is empty");
        return false;
    }
    if (cfg == realCfg || cfg.startsWith(realCfg + QLatin1Char('/'))) {
        msg = QStringLiteral("config location %1 is inside the real %2").arg(cfg, realCfg);
        return false;
    }
    if (!QDir(cfg).absolutePath().startsWith(root)) {
        msg = QStringLiteral("config location %1 is not inside the sandbox %2").arg(cfg, root);
        return false;
    }
    QSettings probe(QStringLiteral("HasanBucak"), QStringLiteral("hb-download-manager-test-guard"));
    if (!probe.fileName().startsWith(settingsRoot)) {
        msg = QStringLiteral("QSettings file %1 is not inside the sandbox").arg(probe.fileName());
        return false;
    }
    return true;
}

inline QString sandboxPath() {
    return QDir(sandboxDir().path()).canonicalPath();
}

} // namespace TestGuard
