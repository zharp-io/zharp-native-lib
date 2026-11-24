cmake_minimum_required(VERSION 3.16)

project(zharp
    VERSION 0.2.0
    DESCRIPTION "Zharp for C and C++: crash reports, errors, logs and traces"
    HOMEPAGE_URL "https://zharp.io/docs/sdks/native"
    LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(ZHARP_TOP_LEVEL ON)
else()
    set(ZHARP_TOP_LEVEL OFF)
endif()

option(ZHARP_CRASHPAD "Report crashes with Crashpad (builds crashpad_handler)" ON)
if(ANDROID)
    # Android's own exit records, read by Zharp for Android, report crashes there.
    set(ZHARP_CRASHPAD OFF CACHE BOOL "" FORCE)
endif()
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
if(ANDROID)
    # On Android the C API forwards to Zharp for Android (io.zharp:android) over JNI: it reads
    # crashes from Android's exit records and sends everything; no Crashpad, no libcurl here.
    add_library(zharp src/android.cpp src/platform.cpp src/stack.cpp)
else()
    add_library(zharp
        src/core.cpp
        src/crash.cpp
        src/platform.cpp
        src/stack.cpp
        src/transport.cpp
        $<IF:$<BOOL:${WIN32}>,src/http_winhttp.cpp,src/http_curl.cpp>)
endif()
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
if(ANDROID)
    target_link_libraries(zharp PRIVATE log)
elseif(WIN32)
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

# ── Qt: zharp::qt ─────────────────────────────────────────────────────────────
# On by default when the project that adds Zharp has found Qt already.
if(TARGET Qt6::Core OR TARGET Qt5::Core)
    set(_zharp_qt_default ON)
else()
    set(_zharp_qt_default OFF)
endif()
option(ZHARP_QT "Build the Qt layer, zharp::qt (Qt 6, or Qt 5.15: Core, Gui, Network)" ${_zharp_qt_default})
set(ZHARP_QT_MAJOR "" CACHE STRING "Qt for zharp::qt: 6 or 5 (empty: the project's, else Qt 6, else Qt 5)")
if(ZHARP_QT)
    if(NOT ZHARP_QT_MAJOR)
        if(TARGET Qt6::Core)
            set(ZHARP_QT_MAJOR 6)
        elseif(TARGET Qt5::Core)
            set(ZHARP_QT_MAJOR 5)
        else()
            find_package(Qt6 QUIET COMPONENTS Core)
            if(Qt6_FOUND)
                set(ZHARP_QT_MAJOR 6)
            else()
                set(ZHARP_QT_MAJOR 5)
            endif()
        endif()
    endif()
    if(ZHARP_QT_MAJOR EQUAL 5)
        find_package(Qt5 5.15 REQUIRED COMPONENTS Core Gui Network)
    else()
        find_package(Qt6 REQUIRED COMPONENTS Core Gui Network)
    endif()
    add_library(zharp_qt src/qt/zharp_qt.cpp include/zharp_qt.h)
    add_library(zharp::qt ALIAS zharp_qt)
    set_target_properties(zharp_qt PROPERTIES
        EXPORT_NAME qt
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_compile_features(zharp_qt PUBLIC cxx_std_17)
    target_compile_definitions(zharp_qt PRIVATE ZHARP_QT_BUILDING)
    target_link_libraries(zharp_qt PUBLIC zharp
        Qt${ZHARP_QT_MAJOR}::Core Qt${ZHARP_QT_MAJOR}::Gui Qt${ZHARP_QT_MAJOR}::Network)
    if(MSVC)
        target_compile_options(zharp_qt PRIVATE /W4 /utf-8)
    else()
        target_compile_options(zharp_qt PRIVATE -Wall -Wextra)
    endif()
endif()

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
    if(ZHARP_QT)
        install(TARGETS zharp_qt EXPORT zharpQtTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES include/zharp_qt.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
        install(EXPORT zharpQtTargets NAMESPACE zharp:: DESTINATION ${ZHARP_INSTALL_CMAKEDIR})
        # qmake: include(<prefix>/share/zharp/zharp_qt.pri)
        if(BUILD_SHARED_LIBS)
            set(ZHARP_PRI_LIBDIR "${CMAKE_INSTALL_LIBDIR}")
            set(ZHARP_PRI_BINDIR "${CMAKE_INSTALL_BINDIR}")
            configure_file(cmake/zharp_qt.pri.in "${CMAKE_CURRENT_BINARY_DIR}/zharp_qt.pri" @ONLY)
            install(FILES "${CMAKE_CURRENT_BINARY_DIR}/zharp_qt.pri" DESTINATION ${CMAKE_INSTALL_DATADIR}/zharp)
        endif()
    endif()
endif()
