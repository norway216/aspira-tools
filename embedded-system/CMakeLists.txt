cmake_minimum_required(VERSION 3.16)
project(embedded-installer
    VERSION 1.0.0
    DESCRIPTION "Embedded Linux Installer/Backup/Restore System"
    LANGUAGES CXX C
)

# ---- C++ Standard ----
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---- Build Options ----
option(BUILD_CORE_SERVICE "Build installer-core daemon" ON)
option(BUILD_CLI          "Build installer-cli tool"       ON)
option(BUILD_TESTS        "Build test suite"               ON)
option(ENABLE_FAULT_INJECT "Enable fault injection hooks"  OFF)
option(ENABLE_SANITIZERS  "Enable ASan/UBSan"              OFF)
option(CROSS_COMPILE_ARM64 "Cross-compile for ARM64"       OFF)

# ---- Compiler Warnings ----
include(cmake/CompilerWarnings.cmake)

# ---- Sanitizers ----
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address -fsanitize=undefined)
endif()

# ---- Fault Injection ----
if(ENABLE_FAULT_INJECT)
    add_compile_definitions(ENABLE_FAULT_INJECT=1)
endif()

# ---- Find Dependencies ----
find_package(Threads REQUIRED)

# yaml-cpp
find_package(yaml-cpp QUIET)
if(NOT yaml-cpp_FOUND)
    message(STATUS "yaml-cpp not found via CMake, trying pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(YAMLCPP REQUIRED yaml-cpp)
endif()

# nlohmann_json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found, trying pkg-config")
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(JSON REQUIRED nlohmann_json)
endif()

# libsodium (Ed25519)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED libsodium)

# libzstd
pkg_check_modules(ZSTD REQUIRED libzstd)

# spdlog
find_package(spdlog QUIET)
if(NOT spdlog_FOUND)
    pkg_check_modules(SPDLOG REQUIRED spdlog)
endif()

# libudev (Linux)
pkg_check_modules(UDEV REQUIRED libudev)

# GTest (optional, for tests)
if(BUILD_TESTS)
    find_package(GTest QUIET)
    if(NOT GTest_FOUND)
        message(STATUS "GTest not found; using bundled minimal test framework")
    endif()
endif()

# ---- Include Directories ----
include_directories(${CMAKE_SOURCE_DIR}/include)

# ---- Subdirectories ----
add_subdirectory(src)

if(BUILD_CLI)
    add_subdirectory(cli)
endif()

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
