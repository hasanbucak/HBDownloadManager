#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QTextStream>
#include <QRandomGenerator>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include "BridgeToken.h"

static bool installNativeHostManifests(const QString &hostBinaryPath) {
    QJsonObject chromeManifest;
    chromeManifest["name"] = "com.hasanbucak.hbdownloadmanager";
    chromeManifest["description"] = "HB Download Manager Native Messaging Host";
    chromeManifest["path"] = hostBinaryPath;
    chromeManifest["type"] = "stdio";

    QJsonArray origins;
    origins.append("chrome-extension://fmlcahmlpfmfnnemigpbbbnmkikepjko/");
    origins.append("chrome-extension://afdfemednfkkmmbpliapapodafgbbeme/");
    chromeManifest["allowed_origins"] = origins;

    QByteArray chromeJson = QJsonDocument(chromeManifest).toJson(QJsonDocument::Indented);

    QString home = QDir::homePath();
    QStringList chromeDirs;
#ifdef Q_OS_MACOS
    QStringList candidateChrome;
    candidateChrome << home + "/Library/Application Support/Google/Chrome"
                    << home + "/Library/Application Support/Chromium"
                    << home + "/Library/Application Support/BraveSoftware/Brave-Browser"
                    << home + "/Library/Application Support/Microsoft Edge"
                    << home + "/Library/Application Support/com.operasoftware.Opera"
                    << home + "/Library/Application Support/com.operasoftware.OperaGX"
                    << home + "/Library/Application Support/Vivaldi";
    for (const QString &cDir : candidateChrome) {
        if (QDir(cDir).exists()) {
            chromeDirs << cDir + "/NativeMessagingHosts";
        }
    }
#else
    QStringList candidateChrome;
    candidateChrome << home + "/.config/google-chrome"
                    << home + "/.config/google-chrome-beta"
                    << home + "/.config/google-chrome-unstable"
                    << home + "/.config/chromium"
                    << home + "/.config/BraveSoftware/Brave-Browser"
                    << home + "/.config/microsoft-edge"
                    << home + "/.config/microsoft-edge-beta"
                    << home + "/.config/microsoft-edge-dev"
                    << home + "/.config/opera"
                    << home + "/.config/opera-beta"
                    << home + "/.config/opera-developer"
                    << home + "/.config/opera-gx"
                    << home + "/.config/vivaldi"
                    << home + "/.config/yandex-browser";
    for (const QString &cDir : candidateChrome) {
        if (QDir(cDir).exists()) {
            chromeDirs << cDir + "/NativeMessagingHosts";
        }
    }
#endif

    for (int i = 0; i < chromeDirs.size(); ++i) {
        QDir().mkpath(chromeDirs.at(i));
        QFile file(chromeDirs.at(i) + "/com.hasanbucak.hbdownloadmanager.json");
        if (file.open(QIODevice::WriteOnly)) {
            file.write(chromeJson);
            file.close();
        }
    }

    // Mozilla Firefox (Linux & macOS)
    QJsonObject mozManifest;
    mozManifest["name"] = "com.hasanbucak.hbdownloadmanager";
    mozManifest["description"] = "HB Download Manager Native Messaging Host";
    mozManifest["path"] = hostBinaryPath;
    mozManifest["type"] = "stdio";

    QJsonArray mozExtensions;
    mozExtensions.append("hbdm@hasanbucak.com");
    mozManifest["allowed_extensions"] = mozExtensions;

    QByteArray mozJson = QJsonDocument(mozManifest).toJson(QJsonDocument::Indented);
    QStringList mozDirs;
#ifdef Q_OS_MACOS
    if (QDir(home + "/Library/Application Support/Mozilla").exists()) {
        mozDirs << home + "/Library/Application Support/Mozilla/NativeMessagingHosts";
    }
#else
    if (QDir(home + "/.mozilla").exists()) {
        mozDirs << home + "/.mozilla/native-messaging-hosts";
    }
#endif

    for (int i = 0; i < mozDirs.size(); ++i) {
        QDir().mkpath(mozDirs.at(i));
        QFile mozFile(mozDirs.at(i) + "/com.hasanbucak.hbdownloadmanager.json");
        if (mozFile.open(QIODevice::WriteOnly)) {
            mozFile.write(mozJson);
            mozFile.close();
        }
    }

    return true;
}

static bool launchMainApplication() {
    QString appPath = QCoreApplication::applicationDirPath() + "/hb-download-manager";
    if (!QFile::exists(appPath)) {
        appPath = "hb-download-manager";
    }
    return QProcess::startDetached(appPath, QStringList());
}

static void sendNativeMessage(const QJsonObject &obj) {
    QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    quint32 len = static_cast<quint32>(body.size());
    std::cout.write(reinterpret_cast<const char*>(&len), 4);
    std::cout.write(body.constData(), body.size());
    std::cout.flush();
}

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    QStringList args = app.arguments();
    if (args.contains("--install") || args.contains("-i")) {
        QString exePath = QCoreApplication::applicationFilePath();
        bool ok = installNativeHostManifests(exePath);
        std::cout << (ok ? "Native host manifests successfully installed.\n"
                         : "Failed to install manifests.\n");
        return ok ? 0 : 1;
    }

    // Native Messaging döngüsü (stdin -> stdout)
    while (std::cin.good()) {
        quint32 len = 0;
        std::cin.read(reinterpret_cast<char*>(&len), 4);
        if (std::cin.gcount() != 4 || len == 0 || len > (1 << 20)) {
            break;
        }

        QByteArray buffer(len, 0);
        std::cin.read(buffer.data(), len);
        if (std::cin.gcount() != static_cast<int>(len)) {
            break;
        }

        QJsonDocument doc = QJsonDocument::fromJson(buffer);
        if (!doc.isObject()) {
            QJsonObject err;
            err["ok"] = false;
            err["error"] = "invalid message";
            sendNativeMessage(err);
            continue;
        }

        QJsonObject msg = doc.object();
        QString action = msg["action"].toString();

        QJsonObject resp;
        if (action == "launchApp") {
            resp["ok"] = launchMainApplication();
        } else if (action == "ping") {
            resp["ok"] = true;
        } else if (action == "getToken") {
            QString token = BridgeToken::getOrCreateToken();
            if (!token.isEmpty()) {
                resp["ok"] = true;
                resp["token"] = token;
            } else {
                resp["ok"] = false;
                resp["error"] = "failed to get bridge token";
            }
        } else {
            resp["ok"] = false;
            resp["error"] = "unknown action";
        }

        sendNativeMessage(resp);
    }

    return 0;
}
