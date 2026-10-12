QT = core testlib network
QT -= gui
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_range_validation

INCLUDEPATH += ../src ../src/core ../src/utils
SOURCES += test_range_validation.cpp \
           ../src/core/SegmentedDownloader.cpp \
           ../src/core/RangePlanner.cpp \
           ../src/core/DownloadItem.cpp \
           ../src/utils/PathUtils.cpp
HEADERS += ../src/core/SegmentedDownloader.h \
           ../src/core/RangePlanner.h \
           ../src/core/DownloadItem.h \
           ../src/core/SegmentInfo.h \
           ../src/utils/PathUtils.h
