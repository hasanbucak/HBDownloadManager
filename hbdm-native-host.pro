QT = core
QT -= gui
CONFIG += c++17 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = hbdm-native-host

OBJECTS_DIR = build_host_obj

SOURCES += src/native_host/native_host_main.cpp
