# Qt examples (ZHARP_QT): the shop as a Qt Widgets app, and in QML with a StackView.
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(_qt Qt${ZHARP_QT_MAJOR})
find_package(${_qt} REQUIRED COMPONENTS Widgets)
find_package(${_qt} QUIET COMPONENTS Qml Quick)

add_executable(qt_shop widgets/main.cpp widgets/shop_window.cpp widgets/shop_window.h)
target_link_libraries(qt_shop PRIVATE zharp::qt ${_qt}::Widgets)
zharp_add_crash_handler(qt_shop)
zharp_upload_debug_files(qt_shop INCLUDE_SOURCES)

if(TARGET ${_qt}::Quick)
    add_executable(qt_qml qml/main.cpp qml/payments.cpp qml/payments.h qml/qml.qrc)
    target_link_libraries(qt_qml PRIVATE zharp::qt ${_qt}::Qml ${_qt}::Quick)
    zharp_add_crash_handler(qt_qml)
    zharp_upload_debug_files(qt_qml INCLUDE_SOURCES)
endif()
