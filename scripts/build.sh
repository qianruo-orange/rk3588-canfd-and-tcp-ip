#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$PROJECT_DIR/build"
BIN_DIR="$PROJECT_DIR/bin"
BIN_NAME="rk3588-canfd-and-tcp-ip"
BIN_PATH="$BIN_DIR/$BIN_NAME"
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

# 构建依赖检查：构建前校验工具链与开发库，缺失时给出安装提示并中止，
# 避免在 cmake / make 阶段才暴露晦涩的报错。
check_deps() {
    echo "[DEPS] checking build dependencies..."
    local missing_cmds=() missing_pkgs=()

    # 工具链
    local cmd
    for cmd in cmake make gcc g++; do
        command -v "$cmd" >/dev/null 2>&1 || missing_cmds+=("$cmd")
    done

    # 开发库：每个条目给出若干候选包名，任一已安装即视为满足
    local desc
    check_pkg() {
        desc="$1"; shift
        local p ok=0
        for p in "$@"; do
            if dpkg -s "$p" >/dev/null 2>&1; then ok=1; break; fi
        done
        [ "$ok" = 1 ] || missing_pkgs+=("$desc")
    }

    check_pkg "libsystemd-dev" libsystemd-dev
    check_pkg "libnl-3-dev" libnl-3-dev
    check_pkg "libnl-route-3-dev" libnl-route-3-dev
    check_pkg "libjpeg 开发库 (libjpeg62-turbo-dev)" libjpeg62-turbo-dev libjpeg-dev
    check_pkg "OpenCV 开发库 (libopencv-dev)" libopencv-dev

    # RKNN NPU 运行时（板载 Rockchip SDK 安装，非 apt 包）
    local rknn_ok=0
    for r in /lib/librknnrt.so /usr/lib/librknnrt.so /usr/lib/aarch64-linux-gnu/librknnrt.so; do
        [ -f "$r" ] && { rknn_ok=1; break; }
    done
    [ "$rknn_ok" = 1 ] || missing_pkgs+=("librknnrt.so (RKNN NPU 运行时)")

    if [ ${#missing_cmds[@]} -gt 0 ] || [ ${#missing_pkgs[@]} -gt 0 ]; then
        echo "[DEPS] 缺少以下依赖："
        for c in "${missing_cmds[@]}"; do echo "  - 命令: $c"; done
        for p in "${missing_pkgs[@]}"; do echo "  - 包: $p"; done
        echo "[DEPS] 安装命令（Debian/Ubuntu）："
        echo "  sudo apt-get install -y build-essential cmake libsystemd-dev \\"
        echo "      libnl-3-dev libnl-route-3-dev libjpeg62-turbo-dev libopencv-dev"
        echo "[DEPS] librknnrt.so 需从 Rockchip 官方 SDK 安装到 /lib 或 /usr/lib。"
        exit 1
    fi

    # 可选依赖：FFmpeg（h264_rkmpp 硬编码后端），缺失时自动回退 V4L2 rkvenc
    if ! dpkg -s libavcodec-dev >/dev/null 2>&1; then
        echo "[DEPS] 提示: 未安装 libavcodec-dev，FFmpeg rkmpp 后端不可用（可选，回退 V4L2）"
    fi
    echo "[DEPS] all required dependencies satisfied"
}

# 清理模式不构建，跳过依赖检查
if [ "${1:-}" != "-C" ] && [ "${1:-}" != "--clean" ]; then
    check_deps
fi

case "${1:-}" in
    -D|--debug)
        echo "=== build $BIN_NAME ==="
        echo "[BUILD] DEBUG mode (-O0 -g)"
        mkdir -p "$BIN_DIR"
        cmake -S "$PROJECT_DIR" -B "$BUILD_DIR" \
            -DCMAKE_BUILD_TYPE=Debug \
            -DDEBUG=ON
        ;;
    -R|--release|"")
        echo "=== build $BIN_NAME ==="
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

echo "[BUILD] done -> $BIN_PATH"
