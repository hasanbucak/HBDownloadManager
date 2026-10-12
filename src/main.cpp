#include <QApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSystemTrayIcon>
#include <unistd.h>
#include "core/DownloadManager.h"
#include "server/LocalServer.h"
#include "utils/ClipboardWatcher.h"
#include "ui/MainWindow.h"
#include "ui/TrayManager.h"
#include "ui/NewDownloadDialog.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("hb-download-manager");
    app.setApplicationDisplayName("HB Download Manager (Linux)");
    app.setApplicationVersion("1.3.0");
    app.setOrganizationName("HasanBucak");
    app.setOrganizationDomain("hasanbucak.com");
    app.setWindowIcon(QIcon(":/icons/app_icon.png"));

    const QString serverName = "hbdm-" + QString::number(getuid());

    // Tekil oturum kontrolü (Single-Instance Guard via QLocalServer)
    QLocalSocket socket;
    socket.connectToServer(serverName);
    if (socket.waitForConnected(500)) {
        // Zaten çalışan bir kopya var! Komut satırı argümanlarını mevcut örneğe ilet
        QStringList args = app.arguments();
        QByteArray payload;
        for (int i = 1; i < args.size(); ++i) {
            QString arg = args.at(i);
            if (arg == "--url" && i + 1 < args.size()) {
                payload.append(args.at(++i).toUtf8()).append('\n');
            } else if (arg.startsWith("http://") || arg.startsWith("https://")) {
                payload.append(arg.toUtf8()).append('\n');
            }
        }
        if (payload.isEmpty()) {
            payload.append('\n');
        }
        socket.write(payload);
        socket.flush();
        socket.waitForBytesWritten(1000);
        return 0;
    }

    // İlk çalışan kopya: bayat soketi temizle ve dinlemeye başla
    QLocalServer::removeServer(serverName);
    QLocalServer localServer(&app);
    if (!localServer.listen(serverName)) {
        qWarning("Yerel tekil soket sunucusu başlatılamadı '%s': %s",
                 qPrintable(serverName), qPrintable(localServer.errorString()));
    }

    // Çekirdek indirme yöneticisi
    DownloadManager &dm = DownloadManager::instance();

    // 127.0.0.1:48213 yerel HTTP köprüsü (Chrome/Firefox eklentisi için)
    LocalServer server(dm.serverPort(), &app);
    bool serverStarted = server.start();
    if (!serverStarted) {
        qWarning("Yerel HTTP köprüsü başlatılamadı: %s", qPrintable(server.errorString()));
    }

    // Pano izleyici (URL kopyalandığında yakalama)
    ClipboardWatcher clipboardWatcher(&app);

    // Ana Arayüz ve Sistem Tepsisi
    MainWindow mainWindow;
    mainWindow.setServerStatus(serverStarted, dm.serverPort(), server.errorString());
    TrayManager trayManager(&mainWindow, &app);
    trayManager.show();

    QObject::connect(&clipboardWatcher, SIGNAL(atUrlDetected(QUrl)),
                     &mainWindow, SLOT(onUrlDetected(QUrl)));

    // İkinci bir kopya açıldığında gelen URL'leri karşıla ve pencereyi öne getir
    QObject::connect(&localServer, &QLocalServer::newConnection, [&localServer, &mainWindow]() {
        while (localServer.hasPendingConnections()) {
            QLocalSocket *clientSocket = localServer.nextPendingConnection();
            if (!clientSocket) continue;

            auto handleData = [&mainWindow, clientSocket]() {
                QByteArray data = clientSocket->readAll();
                if (!data.isEmpty()) {
                    QString content = QString::fromUtf8(data);
                    QStringList lines = content.split('\n', Qt::SkipEmptyParts);
                    for (int i = 0; i < lines.size(); ++i) {
                        QString trimmed = lines.at(i).trimmed();
                        if (trimmed.startsWith("http://") || trimmed.startsWith("https://")) {
                            QUrl url(trimmed);
                            if (url.isValid()) {
                                DownloadItem item(url);
                                DownloadManager::instance().addDownload(item, true);
                            }
                        }
                    }
                }
                mainWindow.show();
                mainWindow.showNormal();
                mainWindow.activateWindow();
                mainWindow.raise();
            };

            if (clientSocket->bytesAvailable() > 0) {
                handleData();
            }
            QObject::connect(clientSocket, &QLocalSocket::readyRead, handleData);
            QObject::connect(clientSocket, &QLocalSocket::disconnected, clientSocket, &QLocalSocket::deleteLater);

            mainWindow.show();
            mainWindow.showNormal();
            mainWindow.activateWindow();
            mainWindow.raise();
        }
    });

    // Komut satırı argümanları kontrolü
    QStringList args = app.arguments();
    bool startMinimized = args.contains("--minimized") || args.contains("--tray");
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        startMinimized = false;
    }

    // Eğer doğrudan link verilmişse: hb-download-manager <url> veya --url <url>
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args.at(i);
        QString urlCandidate;
        if (arg == "--url" && i + 1 < args.size()) {
            urlCandidate = args.at(++i);
        } else if (arg.startsWith("http://") || arg.startsWith("https://")) {
            urlCandidate = arg;
        }
        if (!urlCandidate.isEmpty()) {
            NewDownloadDialog dlg(QUrl(urlCandidate), &mainWindow);
            if (dlg.exec() == QDialog::Accepted) {
                dm.addDownload(dlg.downloadItem(), dlg.shouldStartImmediately());
            }
        }
    }

    if (!startMinimized) {
        mainWindow.show();
    }

    return app.exec();
}
