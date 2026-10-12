QT = core testlib
QT -= gui
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_json_roundtrip

INCLUDEPATH += ../src ../src/core ../src/utils
SOURCES += test_json_roundtrip.cpp \
           ../src/core/DownloadItem.cpp \
           ../src/utils/PathUtils.cpp
HEADERS += ../src/core/DownloadItem.h \
           ../src/core/SegmentInfo.h \
           ../src/utils/PathUtils.h
