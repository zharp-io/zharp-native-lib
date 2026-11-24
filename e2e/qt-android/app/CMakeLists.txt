# A Qt Widgets app for Android with Zharp: the SDK from this checkout (on Android, its C API
# forwards to Zharp for Android, io.zharp:android, which the Gradle build in ../android adds).
cmake_minimum_required(VERSION 3.21)
project(qt_shop_android LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Widgets)
qt_standard_project_setup()

set(ZHARP_QT ON CACHE BOOL "" FORCE)
add_subdirectory(../../.. zharp)

qt_add_executable(qt_shop_android main.cpp cart.cpp cart.h)
target_link_libraries(qt_shop_android PRIVATE zharp::zharp zharp::qt Qt6::Widgets)
set_target_properties(qt_shop_android PROPERTIES
    QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../android"
    QT_ANDROID_PACKAGE_NAME io.zharp.e2e.qtshop)
# Frames with lines for the e2e's own lookup; release apps upload their .sym files instead.
target_compile_options(qt_shop_android PRIVATE -g)
