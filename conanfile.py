"""Conan recipe for Zharp's native SDK (C and C++).

    conan create . --version 0.2.0

The package carries zharp::zharp, Crashpad's client libraries and crashpad_handler (in bin/,
shipped next to your executable).
"""
import os

from conan import ConanFile
from conan.tools.cmake import CMake, CMakeToolchain, cmake_layout
from conan.tools.files import copy
from conan.tools.scm import Git


class ZharpConan(ConanFile):
    name = "zharp"
    version = "0.2.0"
    license = "MIT", "Apache-2.0"
    url = "https://github.com/zharp-io/zharp-native-lib"
    homepage = "https://zharp.io/docs/sdks/native"
    description = "Zharp for C and C++: crash reports through Crashpad, handled errors, breadcrumbs, logs and traces"
    topics = ("crash-reporting", "crashpad", "minidump", "error-monitoring", "observability")
    settings = "os", "arch", "compiler", "build_type"
    options = {"crashpad": [True, False], "qt": [True, False], "fPIC": [True, False]}
    default_options = {"crashpad": True, "qt": False, "fPIC": True}

    def config_options(self):
        if self.settings.os == "Windows":
            del self.options.fPIC

    def requirements(self):
        if self.settings.os != "Windows":
            self.requires("libcurl/[>=7.78 <9]")
            self.requires("zlib/[>=1.2.11 <2]")
        if self.options.qt:
            # zharp::qt: Qt log messages, screens and QNetworkAccessManager spans.
            self.requires("qt/[>=6.5 <7]", transitive_headers=True, transitive_libs=True)

    def layout(self):
        cmake_layout(self)

    def source(self):
        git = Git(self)
        git.clone(url=self.url + ".git", target=".", args=["--recursive", "--branch", f"v{self.version}", "--depth", "1"])

    def generate(self):
        toolchain = CMakeToolchain(self)
        toolchain.variables["ZHARP_CRASHPAD"] = bool(self.options.crashpad)
        toolchain.variables["ZHARP_QT"] = bool(self.options.qt)
        if self.options.qt:
            toolchain.variables["ZHARP_QT_MAJOR"] = 6
        toolchain.variables["ZHARP_BUILD_TESTS"] = False
        toolchain.variables["ZHARP_BUILD_EXAMPLES"] = False
        toolchain.variables["ZHARP_INSTALL"] = True
        toolchain.generate()

    def build(self):
        cmake = CMake(self)
        cmake.configure()
        cmake.build()

    def package(self):
        cmake = CMake(self)
        cmake.install()
        copy(self, "LICENSE", self.source_folder, os.path.join(self.package_folder, "licenses"))
        copy(self, "NOTICE", self.source_folder, os.path.join(self.package_folder, "licenses"))

    def package_info(self):
        # find_package(zharp) uses the CMake package the build installs, with its helpers.
        self.cpp_info.set_property("cmake_find_mode", "none")
        self.cpp_info.builddirs = [os.path.join("lib", "cmake", "zharp")]
        self.buildenv_info.prepend_path("PATH", os.path.join(self.package_folder, "bin"))
