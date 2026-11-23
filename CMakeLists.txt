cmake_minimum_required(VERSION 3.16)

project(zharp
    VERSION 0.1.0
    DESCRIPTION "Zharp for C and C++: crash reports, errors, logs and traces"
    HOMEPAGE_URL "https://zharp.io/docs/sdks/native"
    LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ZHARP_TOP_LEVEL ON)
else()
    set(ZHARP_TOP_LEVEL OFF)
endif()

option(ZHARP_CRASHPAD "Report crashes with Crashpad (builds crashpad_handler)" ON)
option(ZHARP_BUILD_TESTS "Build the SDK's tests" ${ZHARP_TOP_LEVEL})
option(ZHARP_BUILD_EXAMPLES "Build the examples" ${ZHARP_TOP_LEVEL})
option(ZHARP_INSTALL "Install the SDK, Crashpad and the CMake package" ${ZHARP_TOP_LEVEL})
set(ZHARP_CRASHPAD_SOURCE_DIR "" CACHE PATH "A Crashpad (getsentry/crashpad) checkout to build instead of external/crashpad")

# The Crashpad fork and commit this release is tested with (see README.md, "Crashpad").
set(ZHARP_CRASHPAD_REPOSITORY "https://github.com/getsentry/crashpad.git")
set(ZHARP_CRASHPAD_COMMIT "bc334efe17f07a56e3219a0261d2862d280d4d34")

include(GNUInstallDirs)
find_package(Threads REQUIRED)

if(BUILD_SHARED_LIBS)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# ── Crashpad ──────────────────────────────────────────────────────────────────
if(ZHARP_CRASHPAD)
    if(ZHARP_CRASHPAD_SOURCE_DIR)
        set(_zharp_crashpad_dir "${ZHARP_CRASHPAD_SOURCE_DIR}")
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/crashpad/CMakeLists.txt")
        set(_zharp_crashpad_dir "${CMAKE_CURRENT_SOURCE_DIR}/external/crashpad")
    else()
        # A copy without the submodule (a source archive): fetch the pinned commit.
        include(FetchContent)
        FetchContent_Declare(zharp_crashpad
            GIT_REPOSITORY "${ZHARP_CRASHPAD_REPOSITORY}"
            GIT_TAG "${ZHARP_CRASHPAD_COMMIT}"
            GIT_SUBMODULES_RECURSE ON)
        FetchContent_GetProperties(zharp_crashpad)
        if(NOT zharp_crashpad_POPULATED)
            FetchContent_Populate(zharp_crashpad)
        endif()
        set(_zharp_crashpad_dir "${zharp_crashpad_SOURCE_DIR}")
    endif()
    set(CRASHPAD_ENABLE_INSTALL ${ZHARP_INSTALL} CACHE BOOL "" FORCE)
    set(CRASHPAD_ENABLE_INSTALL_DEV ${ZHARP_INSTALL} CACHE BOOL "" FORCE)
    add_subdirectory("${_zharp_crashpad_dir}" "${CMAKE_CURRENT_BINARY_DIR}/crashpad")
endif()

# ── The SDK ───────────────────────────────────────────────────────────────────
add_library(zharp
    src/core.cpp
    src/crash.cpp
    src/platform.cpp
    src/stack.cpp
    src/transport.cpp
    $<IF:$<BOOL:${WIN32}>,src/http_winhttp.cpp,src/http_curl.cpp>)
add_library(zharp::zharp ALIAS zharp)
set_target_properties(zharp PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_compile_features(zharp PRIVATE cxx_std_17)
target_include_directories(zharp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_compile_definitions(zharp PRIVATE ZHARP_BUILDING)
if(BUILD_SHARED_LIBS)
    target_compile_definitions(zharp PUBLIC ZHARP_SHARED)
endif()
target_link_libraries(zharp PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
    # Crashpad's headers pull in windows.h; its min/max macros break mini_chromium's span.h.
    target_compile_definitions(zharp PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(zharp PRIVATE winhttp psapi)
else()
    find_package(CURL REQUIRED)
    target_link_libraries(zharp PRIVATE CURL::libcurl)
endif()
if(ZHARP_CRASHPAD)
    target_compile_definitions(zharp PRIVATE ZHARP_WITH_CRASHPAD)
    target_link_libraries(zharp PRIVATE crashpad::client)
endif()
if(MSVC)
    target_compile_options(zharp PRIVATE /W4 /utf-8)
else()
    target_compile_options(zharp PRIVATE -Wall -Wextra)
endif()

set(ZHARP_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake" CACHE INTERNAL "")
include(cmake/zharpFunctions.cmake)

# ── Tests and examples ────────────────────────────────────────────────────────
if(ZHARP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
if(ZHARP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ── Install: find_package(zharp) ──────────────────────────────────────────────
if(ZHARP_INSTALL)
    include(CMakePackageConfigHelpers)
    set(ZHARP_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/zharp")
    install(TARGETS zharp EXPORT zharpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    install(FILES include/zharp.h include/zharp.hpp include/zharp_curl.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(EXPORT zharpTargets NAMESPACE zharp:: DESTINATION ${ZHARP_INSTALL_CMAKEDIR})
    configure_package_config_file(cmake/zharpConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/zharpConfig.cmake"
        INSTALL_DESTINATION ${ZHARP_INSTALL_CMAKEDIR})
    write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/zharpConfigVersion.cmake"
        COMPATIBILITY SameMinorVersion)
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/zharpConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/zharpConfigVersion.cmake"
        cmake/zharpFunctions.cmake
        cmake/zharp-upload-debug-files.cmake
        DESTINATION ${ZHARP_INSTALL_CMAKEDIR})
    install(FILES LICENSE NOTICE DESTINATION ${CMAKE_INSTALL_DATADIR}/zharp)
endif()
