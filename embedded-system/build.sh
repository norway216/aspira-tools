#!/bin/bash
# Build the embedded Linux installer core.
# Requires: g++, libyaml-cpp, libsodium, libzstd, libudev, libpthread
#
# Usage: ./build.sh [debug|release]
# Output: ./installer-core

set -e
cd "$(dirname "$0")"

BUILD_TYPE="${1:-release}"
CXXFLAGS="-std=c++17 -I. -Iinclude"
LDFLAGS="-lyaml-cpp -lsodium -lzstd -ludev -lpthread"

# Use locally-extracted headers if available
if [ -d /tmp/installer-deps/include ]; then
    CXXFLAGS="$CXXFLAGS -I/tmp/installer-deps/include"
    LDFLAGS="-L/tmp/installer-deps/lib/x86_64-linux-gnu $LDFLAGS"
fi

if [ "$BUILD_TYPE" = "debug" ]; then
    CXXFLAGS="$CXXFLAGS -O0 -g -DDEBUG"
else
    CXXFLAGS="$CXXFLAGS -O2"
fi

echo "=== Embedded Linux Installer Core ==="
echo "Build type: $BUILD_TYPE"
echo ""

g++ $CXXFLAGS $LDFLAGS \
    -o installer-core \
    src/app/standalone_main.cpp \
    src/log/minimal_logger.cpp \
    src/common/error_codes.cpp \
    src/common/file_utils.cpp \
    src/common/types.cpp \
    src/platform/process_runner.cpp \
    src/config/config_loader.cpp \
    src/core/device_manager.cpp \
    src/core/security_manager.cpp \
    src/core/package_manager.cpp \
    src/core/image_writer.cpp \
    src/core/partition_manager.cpp \
    src/core/filesystem_manager.cpp \
    src/core/boot_control.cpp \
    src/core/hardware_profile.cpp

echo ""
echo "Build successful: $(du -h installer-core | cut -f1)"
echo "Run: ./installer-core"
