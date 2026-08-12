#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"

usage() {
    echo "Usage: $0 [-D|--debug] [-R|--release] [-C|--clean]"
    echo "  -D, --debug     build with debug output to terminal"
    echo "  -R, --release   build for production (default)"
    echo "  -C, --clean     remove build directory"
    exit 1
}

echo "=== build data_transport_test ==="

case "${1:-}" in
    -D|--debug)
        echo "[BUILD] DEBUG mode (-O0 -g)"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DDEBUG=ON
        ;;
    -R|--release|"")
        echo "[BUILD] RELEASE mode (-O3 -DNDEBUG)"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Release
        ;;
    -C|--clean)
        echo "[CLEAN] removing $BUILD_DIR"
        rm -rf "$BUILD_DIR"
        echo "[CLEAN] done"
        exit 0
        ;;
    *)
        usage
        ;;
esac

make -C "$BUILD_DIR" -j"$(nproc)"
echo "[BUILD] done -> $PROJECT_DIR/bin/data_transport_test"
