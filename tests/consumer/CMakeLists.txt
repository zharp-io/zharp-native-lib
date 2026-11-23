# A fresh project using the installed SDK: find_package(zharp), the crash handler next to the
# executable, and a run that reports a handled error with Zharp off (no key).
cmake_minimum_required(VERSION 3.16)
project(zharp_consumer LANGUAGES C CXX)

find_package(zharp 0.1 CONFIG REQUIRED)

add_executable(consumer main.cpp)
target_compile_features(consumer PRIVATE cxx_std_17)
target_link_libraries(consumer PRIVATE zharp::zharp)
zharp_add_crash_handler(consumer)

add_executable(consumer_c main.c)
target_link_libraries(consumer_c PRIVATE zharp::zharp)

enable_testing()
add_test(NAME consumer COMMAND consumer)
add_test(NAME consumer_c COMMAND consumer_c)
add_test(NAME handler_next_to_it COMMAND ${CMAKE_COMMAND} -E sha256sum "$<TARGET_FILE_DIR:consumer>/$<IF:$<BOOL:${WIN32}>,crashpad_handler.exe,crashpad_handler>")
