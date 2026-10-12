QT += core
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = hbdm-native-host

OBJECTS_DIR = build_host_obj
LIBS += -L$$PWD/lib -lGL

SOURCES += src/native_host/native_host_main.cpp
