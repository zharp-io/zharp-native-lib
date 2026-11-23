# The Qt layer (ZHARP_QT) on Qt's offscreen platform, against the stand-in OTLP receiver.
add_executable(zharp_qt_tests qt_tests.cpp)
target_link_libraries(zharp_qt_tests PRIVATE zharp::qt Threads::Threads)
if(WIN32)
    target_link_libraries(zharp_qt_tests PRIVATE ws2_32)
endif()
add_test(NAME qt COMMAND zharp_qt_tests)
set_tests_properties(qt PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
