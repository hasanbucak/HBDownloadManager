QT += core gui widgets network
CONFIG += c++17
TEMPLATE = app
TARGET = hb-download-manager

INCLUDEPATH += src src/core src/server src/ui src/utils
LIBS += -L$$PWD/lib -lGL

HEADERS += src/core/SegmentInfo.h \
           src/core/DownloadItem.h \
           src/core/SegmentedDownloader.h \
           src/core/VideoDownloader.h \
           src/core/DownloadManager.h \
           src/server/LocalServer.h \
           src/utils/ClipboardWatcher.h \
           src/ui/NewDownloadDialog.h \
           src/ui/DownloadDetailDialog.h \
           src/ui/MainWindow.h \
           src/ui/TrayManager.h

SOURCES += src/main.cpp \
           src/core/DownloadItem.cpp \
           src/core/SegmentedDownloader.cpp \
           src/core/VideoDownloader.cpp \
           src/core/DownloadManager.cpp \
           src/server/LocalServer.cpp \
           src/utils/ClipboardWatcher.cpp \
           src/ui/NewDownloadDialog.cpp \
           src/ui/DownloadDetailDialog.cpp \
           src/ui/MainWindow.cpp \
           src/ui/TrayManager.cpp

RESOURCES += resources/resources.qrc
