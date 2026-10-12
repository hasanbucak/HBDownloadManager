QT = core testlib
QT -= gui
CONFIG += c++17 testcase
TEMPLATE = app
TARGET = test_pathutils

INCLUDEPATH += ../src ../src/utils
SOURCES += test_pathutils.cpp ../src/utils/PathUtils.cpp
HEADERS += ../src/utils/PathUtils.h
