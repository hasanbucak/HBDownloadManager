QT += core gui widgets network
CONFIG += c++17
TEMPLATE = app
TARGET = hb-download-manager

# One version for everything: the VERSION file (see scripts/check_version.sh)
HBDM_VERSION_STR = $$cat($$PWD/VERSION)
DEFINES += HBDM_VERSION=\\\"$$HBDM_VERSION_STR\\\"

INCLUDEPATH += src src/core src/server src/ui src/utils
QMAKE_LIBDIR += $$[QT_INSTALL_LIBS]

HEADERS += src/core/SegmentInfo.h \
           src/core/DownloadItem.h \
           src/core/RangePlanner.h \
           src/core/SegmentedDownloader.h \
           src/core/VideoDownloader.h \
           src/core/DownloadManager.h \
           src/server/LocalServer.h \
           src/utils/PathUtils.h \
           src/utils/MediaHosts.h \
           src/utils/ClipboardWatcher.h \
           src/ui/NewDownloadDialog.h \
           src/ui/DownloadDetailDialog.h \
           src/ui/SettingsDialog.h \
           src/ui/MainWindow.h \
           src/ui/TrayManager.h

SOURCES += src/main.cpp \
           src/core/DownloadItem.cpp \
           src/core/RangePlanner.cpp \
           src/core/SegmentedDownloader.cpp \
           src/core/VideoDownloader.cpp \
           src/core/DownloadManager.cpp \
           src/server/LocalServer.cpp \
           src/utils/PathUtils.cpp \
           src/utils/MediaHosts.cpp \
           src/utils/ClipboardWatcher.cpp \
           src/ui/NewDownloadDialog.cpp \
           src/ui/DownloadDetailDialog.cpp \
           src/ui/SettingsDialog.cpp \
           src/ui/MainWindow.cpp \
           src/ui/TrayManager.cpp

RESOURCES += resources/resources.qrc

# Translations: Turkish is the source language, the English catalog is compiled by lrelease and
# embedded in the binary under :/i18n/ (needs Qt 5.12 or newer)
CONFIG += lrelease embed_translations
TRANSLATIONS += translations/hbdm_en.ts
