QT += core widgets network testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_download_lifecycle

INCLUDEPATH += ../src ../src/core ../src/server ../src/ui ../src/utils
SOURCES += test_download_lifecycle.cpp \
           mock_downloaders.cpp \
           ../src/utils/PathUtils.cpp \
           ../src/utils/MediaHosts.cpp \
           ../src/core/DownloadItem.cpp \
           ../src/core/DownloadManager.cpp
HEADERS += ../src/core/DownloadManager.h \
           ../src/core/DownloadItem.h \
           ../src/core/SegmentedDownloader.h \
           ../src/core/VideoDownloader.h \
           ../src/utils/PathUtils.h \
           ../src/utils/MediaHosts.h
