#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN_DIR="$PROJECT_DIR/bin"
BIN_PATH="$BIN_DIR/data_transport_test"
CMAKE_DIR="$PROJECT_DIR/cmake"
LOGS_DIR="$PROJECT_DIR/logs"

usage() {
    echo "Usage: $0 [-D|--debug] [-R|--release] [-C|--clean]"
    echo "  -D, --debug     build with debug output to terminal"
    echo "  -R, --release   build for production (default)"
    echo "  -C, --clean     remove build directory, bin directory, cmake directory, generated CMake artifacts, and clear logs"
    exit 1
}

print_version_info() {
    local version_header="$PROJECT_DIR/include/core/version.h"

    if [ -f "$version_header" ]; then
        local app_version app_build_type
        app_version=$(grep '^#define APP_VERSION ' "$version_header" | head -n 1 | cut -d'"' -f2)
        app_build_type=$(grep '^#define APP_BUILD_TYPE ' "$version_header" | head -n 1 | cut -d'"' -f2)
        echo "[BUILD] version=${app_version:-unknown} build_type=${app_build_type:-unknown}"
    else
        echo "[BUILD] version header not generated yet"
    fi
}

clean_build() {
    echo "[CLEAN] removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"

    if [ -d "$BIN_DIR" ]; then
        echo "[CLEAN] removing $BIN_DIR"
        rm -rf "$BIN_DIR"
    else
        echo "[CLEAN] no $BIN_DIR to remove"
    fi

    for artifact in CMakeCache.txt CMakeFiles cmake_install.cmake Makefile; do
        if [ -e "$PROJECT_DIR/$artifact" ]; then
            echo "[CLEAN] removing $PROJECT_DIR/$artifact"
            rm -rf "$PROJECT_DIR/$artifact"
        else
            echo "[CLEAN] no $PROJECT_DIR/$artifact to remove"
        fi
    done

    if [ -d "$CMAKE_DIR" ]; then
        echo "[CLEAN] removing $CMAKE_DIR"
        rm -rf "$CMAKE_DIR"
    else
        echo "[CLEAN] no $CMAKE_DIR to remove"
    fi

    if [ -f "$PROJECT_DIR/include/core/version.h" ]; then
        echo "[CLEAN] removing $PROJECT_DIR/include/core/version.h"
        rm -f "$PROJECT_DIR/include/core/version.h"
    else
        echo "[CLEAN] no version header to remove"
    fi

    echo "[CLEAN] removing $LOGS_DIR"
    rm -rf "$LOGS_DIR"
    mkdir -p "$LOGS_DIR"

    echo "[CLEAN] done"
}

case "${1:-}" in
    -D|--debug)
        echo "=== build data_transport_test ==="
        echo "[BUILD] DEBUG mode (-O0 -g)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DDEBUG=ON
        ;;
    -R|--release|"")
        echo "=== build data_transport_test ==="
        echo "[BUILD] RELEASE mode (-O3 -DNDEBUG)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release
        ;;
    -C|--clean)
        clean_build
        exit 0
        ;;
    *)
        usage
        ;;
esac

make -C "$BUILD_DIR" -j"$(nproc)"
print_version_info

echo "[BUILD] done -> $PROJECT_DIR/bin/data_transport_test"
