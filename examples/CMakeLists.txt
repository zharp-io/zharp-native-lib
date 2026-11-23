# A C++ shop that crashes in several ways (and reports a handled error), and a plain C program.
add_executable(shop shop/main.cpp shop/cart.cpp shop/shop.cpp)
target_compile_features(shop PRIVATE cxx_std_17)
target_link_libraries(shop PRIVATE zharp::zharp)
zharp_add_crash_handler(shop)
zharp_upload_debug_files(shop INCLUDE_SOURCES)

add_executable(hello_c c/hello.c)
target_link_libraries(hello_c PRIVATE zharp::zharp)

if(ZHARP_QT)
    add_subdirectory(qt)
endif()
