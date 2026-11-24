# The Qt layer (ZHARP_QT) on Qt's offscreen platform, against the stand-in OTLP receiver.
add_executable(zharp_qt_tests qt_tests.cpp)
target_link_libraries(zharp_qt_tests PRIVATE zharp::qt Threads::Threads)
if(WIN32)
    target_link_libraries(zharp_qt_tests PRIVATE ws2_32)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
    # The SDK's DLLs next to the test, as Windows looks for them there (Qt's come from PATH).
    add_custom_command(TARGET zharp_qt_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zharp> $<TARGET_FILE:zharp_qt> $<TARGET_FILE_DIR:zharp_qt_tests>)
endif()
add_test(NAME qt COMMAND zharp_qt_tests)
set_tests_properties(qt PROPERTIES TIMEOUT 120 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
