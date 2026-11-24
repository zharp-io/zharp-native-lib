# CMake helpers for apps using Zharp. Available after add_subdirectory / FetchContent of the SDK,
# or find_package(zharp).

# zharp_add_crash_handler(<target>)
#   Copies Crashpad's handler (crashpad_handler, crashpad_handler.exe on Windows) next to the
#   target's executable after each build: zharp_init finds it there. Install it next to your
#   executable too: install(PROGRAMS $<TARGET_FILE:zharp::crashpad_handler> DESTINATION bin).
function(zharp_add_crash_handler target)
    if(ANDROID)
        return()  # Zharp for Android reads crashes from Android's exit records: no handler to ship.
    endif()
    if(TARGET crashpad_handler)
        set(_handler crashpad_handler)
    elseif(TARGET crashpad::crashpad_handler)
        set(_handler crashpad::crashpad_handler)
    else()
        message(WARNING "zharp_add_crash_handler: Zharp was built without Crashpad (ZHARP_CRASHPAD=OFF); crashes won't be reported")
        return()
    endif()
    if(NOT TARGET zharp::crashpad_handler)
        add_executable(zharp::crashpad_handler ALIAS ${_handler})
    endif()
    if(TARGET crashpad_handler)
        add_dependencies(${target} crashpad_handler)
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${_handler}>" "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Zharp: crashpad_handler next to ${target}"
        VERBATIM)
endfunction()

# zharp_upload_debug_files(<target> [APP <id>] [INCLUDE_SOURCES] [CONFIGS <config>...])
#   After each Release build (Release, RelWithDebInfo, MinSizeRel by default), uploads the
#   target's debug information (its PDB on Windows, the binary's DWARF elsewhere) as Breakpad .sym
#   files with `npx @zharp/cli debug-files upload`, so its crashes show function names, files and
#   lines. Needs Node.js and ZHARP_AUTH_TOKEN (a workspace API key with "artifacts:write") in the
#   build's environment; without them the build goes on and says it skipped the upload.
#   APP defaults to the ZHARP_APP environment variable. INCLUDE_SOURCES also uploads the source
#   files the symbols name, so crashes show the code that ran.
function(zharp_upload_debug_files target)
    cmake_parse_arguments(ARG "INCLUDE_SOURCES" "APP" "CONFIGS" ${ARGN})
    if(NOT ARG_CONFIGS)
        set(ARG_CONFIGS Release RelWithDebInfo MinSizeRel)
    endif()
    set(_files "$<TARGET_FILE:${target}>")
    if(MSVC)
        list(APPEND _files "$<TARGET_PDB_FILE:${target}>")
    endif()
    string(REPLACE ";" "|" _files "${_files}")
    string(REPLACE ";" "|" _configs "${ARG_CONFIGS}")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            "-DZHARP_FILES=${_files}"
            "-DZHARP_APP=${ARG_APP}"
            "-DZHARP_CONFIG=$<CONFIG>"
            "-DZHARP_CONFIGS=${_configs}"
            "-DZHARP_INCLUDE_SOURCES=${ARG_INCLUDE_SOURCES}"
            -P "${ZHARP_CMAKE_DIR}/zharp-upload-debug-files.cmake"
        VERBATIM)
endfunction()
