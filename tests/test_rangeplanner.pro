QT = core testlib
QT -= gui
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_rangeplanner

INCLUDEPATH += ../src ../src/core
SOURCES += test_rangeplanner.cpp ../src/core/RangePlanner.cpp
HEADERS += ../src/core/RangePlanner.h ../src/core/SegmentInfo.h
