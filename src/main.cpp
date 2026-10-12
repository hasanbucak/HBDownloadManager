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

#include <QLockFile>
#include <QDir>
#include <QStandardPaths>
#include <QSocketNotifier>
#include <csignal>
#include <sys/socket.h>

static int s_sigFd[2] = {-1, -1};
static void onUnixSignal(int) {
    const char c = 1;
    ssize_t r = ::write(s_sigFd[0], &c, 1);
    (void)r;
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    app.setApplicationName("hb-download-manager");
    app.setApplicationDisplayName("HB Download Manager (Linux)");
    app.setApplicationVersion("1.3.0");
    app.setOrganizationName("HasanBucak");
    app.setOrganizationDomain("hasanbucak.com");
    app.setWindowIcon(QIcon(":/icons/app_icon.png"));

    // D6: Self-pipe pattern for clean termination on SIGTERM, SIGHUP, SIGINT
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, s_sigFd) == 0) {
        QSocketNotifier *sigNotifier = new QSocketNotifier(s_sigFd[1], QSocketNotifier::Read, &app);
        QObject::connect(sigNotifier, &QSocketNotifier::activated, &app, [&app]() {
            char c;
            ssize_t r = ::read(s_sigFd[1], &c, 1);
            (void)r;
            app.quit();
        });

        struct sigaction sa;
        ::memset(&sa, 0, sizeof(sa));
        sa.sa_handler = onUnixSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &sa, NULL);
        ::sigaction(SIGHUP, &sa, NULL);
        ::sigaction(SIGINT, &sa, NULL);
    }

    QString runtimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty() || !QDir(runtimeDir).exists()) {
        runtimeDir = QDir::tempPath();
    }
    const QString lockPath = runtimeDir + "/hb-download-manager.lock";
    const QString sockPath = runtimeDir + "/hb-download-manager.sock";

    QLockFile lockFile(lockPath);
    lockFile.setStaleLockTime(10000);
    bool isFirstInstance = lockFile.tryLock(500);

    if (!isFirstInstance) {
        // Zaten çalışan bir kopya var! Komut satırı argümanlarını mevcut örneğe ilet
        QLocalSocket socket;
        socket.connectToServer(sockPath);
        if (socket.waitForConnected(1000)) {
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
            socket.disconnectFromServer();
        }
        return 0;
    }

    // İlk çalışan kopya: bayat soketi temizle ve dinlemeye başla
    QLocalServer::removeServer(sockPath);
    QLocalServer localServer(&app);
    localServer.setSocketOptions(QLocalServer::UserAccessOption);
    if (!localServer.listen(sockPath)) {
        qWarning("Yerel tekil soket sunucusu başlatılamadı '%s': %s",
                 qPrintable(sockPath), qPrintable(localServer.errorString()));
    }

    // Çekirdek indirme yöneticisi
    DownloadManager &dm = DownloadManager::instance();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &dm, &DownloadManager::shutdown);

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

            QByteArray *buffer = new QByteArray();
            QObject::connect(clientSocket, &QLocalSocket::readyRead, [clientSocket, buffer]() {
                buffer->append(clientSocket->readAll());
            });
            QObject::connect(clientSocket, &QLocalSocket::disconnected, [&mainWindow, clientSocket, buffer]() {
                buffer->append(clientSocket->readAll());
                QString content = QString::fromUtf8(*buffer);
                delete buffer;
                clientSocket->deleteLater();

                QStringList lines = content.split('\n', Qt::SkipEmptyParts);
                for (int i = 0; i < lines.size(); ++i) {
                    QString trimmed = lines.at(i).trimmed();
                    if (trimmed.startsWith("http://") || trimmed.startsWith("https://")) {
                        QUrl url(trimmed);
                        if (url.isValid()) {
                            mainWindow.onUrlDetected(url);
                        }
                    }
                }
                mainWindow.show();
                mainWindow.showNormal();
                mainWindow.activateWindow();
                mainWindow.raise();
            });

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
