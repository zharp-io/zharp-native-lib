# vcpkg port for Zharp's native SDK. GitHub's source archives leave out submodules, so Crashpad
# (getsentry/crashpad at the commit CMakeLists.txt pins) and its own submodules are fetched here
# and handed to the build with ZHARP_CRASHPAD_SOURCE_DIR.
#
# The SHA512 values are filled in at release: run `vcpkg install zharp --overlay-ports=ports/vcpkg`
# once and copy the hashes vcpkg prints.

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO zharp-io/zharp-native-lib
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main
)

vcpkg_from_github(
    OUT_SOURCE_PATH CRASHPAD_PATH
    REPO getsentry/crashpad
    REF bc334efe17f07a56e3219a0261d2862d280d4d34
    SHA512 0
)
vcpkg_from_github(
    OUT_SOURCE_PATH MINI_CHROMIUM_PATH
    REPO getsentry/mini_chromium
    REF 2f5c168fa462cf3836c4a7801b4f4955496d975d
    SHA512 0
)
vcpkg_from_github(
    OUT_SOURCE_PATH ZLIB_PATH
    REPO getsentry/chromium-zlib
    REF fef58692c1d7bec94c4ed3d030a45a1832a9615d
    SHA512 0
)
vcpkg_from_github(
    OUT_SOURCE_PATH LSS_PATH
    REPO getsentry/chromium-linux-syscall-support
    REF 9719c1e1e676814c456b55f5f070eabad6709d31
    SHA512 0
)
file(REMOVE_RECURSE "${CRASHPAD_PATH}/third_party/mini_chromium/mini_chromium" "${CRASHPAD_PATH}/third_party/zlib/zlib" "${CRASHPAD_PATH}/third_party/lss/lss")
file(RENAME "${MINI_CHROMIUM_PATH}" "${CRASHPAD_PATH}/third_party/mini_chromium/mini_chromium")
file(RENAME "${ZLIB_PATH}" "${CRASHPAD_PATH}/third_party/zlib/zlib")
file(RENAME "${LSS_PATH}" "${CRASHPAD_PATH}/third_party/lss/lss")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DZHARP_CRASHPAD_SOURCE_DIR=${CRASHPAD_PATH}
        -DZHARP_BUILD_TESTS=OFF
        -DZHARP_BUILD_EXAMPLES=OFF
        -DZHARP_INSTALL=ON
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME zharp CONFIG_PATH lib/cmake/zharp DO_NOT_DELETE_PARENT_CONFIG_PATH)
vcpkg_cmake_config_fixup(PACKAGE_NAME crashpad CONFIG_PATH lib/cmake/crashpad)

# crashpad_handler goes with the app: vcpkg keeps executables under tools/.
vcpkg_copy_tools(TOOL_NAMES crashpad_handler AUTO_CLEAN)

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include" "${CURRENT_PACKAGES_DIR}/debug/share")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE" "${SOURCE_PATH}/NOTICE")
