QT += core widgets network testlib
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_bridge_auth

INCLUDEPATH += ../src ../src/core ../src/server ../src/ui ../src/utils
SOURCES += test_bridge_auth.cpp \
           mock_downloaders.cpp \
           ../src/utils/PathUtils.cpp \
           ../src/utils/MediaHosts.cpp \
           ../src/core/DownloadItem.cpp \
           ../src/core/DownloadManager.cpp \
           ../src/server/LocalServer.cpp
HEADERS += ../src/server/LocalServer.h \
           ../src/core/DownloadManager.h \
           ../src/core/DownloadItem.h \
           ../src/core/SegmentedDownloader.h \
           ../src/core/VideoDownloader.h \
           ../src/utils/PathUtils.h \
           ../src/utils/MediaHosts.h
