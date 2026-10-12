#include <QApplication>
#include <QIcon>
#include "core/DownloadManager.h"
#include "server/LocalServer.h"
#include "utils/ClipboardWatcher.h"
#include "ui/MainWindow.h"
#include "ui/TrayManager.h"
#include "ui/NewDownloadDialog.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("hb-download-manager");
    app.setApplicationDisplayName("HB Download Manager (Linux)");
    app.setApplicationVersion("1.3.0");
    app.setOrganizationName("HasanBucak");
    app.setOrganizationDomain("hasanbucak.com");
    app.setWindowIcon(QIcon(":/icons/app_icon.png"));

    // Çekirdek indirme yöneticisi
    DownloadManager &dm = DownloadManager::instance();

    // 127.0.0.1:48213 yerel HTTP köprüsü (Chrome/Firefox eklentisi için)
    LocalServer server(dm.serverPort(), &app);
    server.start();

    // Pano izleyici (URL kopyalandığında yakalama)
    ClipboardWatcher clipboardWatcher(&app);

    // Ana Arayüz ve Sistem Tepsisi
    MainWindow mainWindow;
    TrayManager trayManager(&mainWindow, &app);
    trayManager.show();

    QObject::connect(&clipboardWatcher, SIGNAL(atUrlDetected(QUrl)),
                     &mainWindow, SLOT(onUrlDetected(QUrl)));

    // Komut satırı argümanları kontrolü
    QStringList args = app.arguments();
    bool startMinimized = args.contains("--minimized") || args.contains("--tray");

    // Eğer doğrudan link verilmişse: hb-download-manager <url>
    for (int i = 1; i < args.size(); ++i) {
        QString arg = args.at(i);
        if (arg.startsWith("http://") || arg.startsWith("https://")) {
            NewDownloadDialog dlg(QUrl(arg), &mainWindow);
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
