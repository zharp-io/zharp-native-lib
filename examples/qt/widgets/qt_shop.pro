# The widgets shop with qmake, against an installed shared build of the SDK:
#   qmake ZHARP_PREFIX=<prefix> && make
QT += widgets
TARGET = qt_shop
SOURCES += main.cpp shop_window.cpp
HEADERS += shop_window.h
CONFIG += force_debug_info
include($$ZHARP_PREFIX/share/zharp/zharp_qt.pri)
