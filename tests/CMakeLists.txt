# Unit tests of the internals (static builds only: they call into src/), and an end-to-end test
# against a stand-in OTLP receiver on 127.0.0.1.
if(NOT BUILD_SHARED_LIBS)
    add_executable(zharp_unit_tests unit.cpp)
    target_compile_features(zharp_unit_tests PRIVATE cxx_std_17)
    target_include_directories(zharp_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(zharp_unit_tests PRIVATE zharp::zharp)
    add_test(NAME unit COMMAND zharp_unit_tests)
endif()

add_executable(zharp_integration_tests integration.cpp receiver.h)
target_compile_features(zharp_integration_tests PRIVATE cxx_std_17)
target_link_libraries(zharp_integration_tests PRIVATE zharp::zharp Threads::Threads)
if(WIN32)
    target_link_libraries(zharp_integration_tests PRIVATE ws2_32)
endif()
if(WIN32 AND BUILD_SHARED_LIBS)
    # Windows finds a DLL next to the executable (there is no rpath): without it the test can't
    # start, and the loader's error box waits out the timeout.
    add_custom_command(TARGET zharp_integration_tests POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:zharp> $<TARGET_FILE_DIR:zharp_integration_tests>)
endif()
add_test(NAME integration COMMAND zharp_integration_tests)
set_tests_properties(integration PROPERTIES TIMEOUT 120)

if(ZHARP_QT)
    add_subdirectory(qt)
endif()
