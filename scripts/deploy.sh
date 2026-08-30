# SPDX-License-Identifier: GPL-3.0-or-later
#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
APP_NAME="rk3588-edge-gateway"
DEPLOY_DIR="/opt/$APP_NAME"
BINARY="$PROJECT_DIR/bin/$APP_NAME"
SERVICE_SRC="$PROJECT_DIR/$APP_NAME.service"
SERVICE_NAME="$APP_NAME"

usage() {
    echo "Usage: $0 [-i | -u | -r]"
    echo "  -i   Install: deploy and start service (default)"
    echo "  -u   Uninstall: stop service, remove files"
    echo "  -r   Reinstall: uninstall then install"
    echo "  -h   Show this help"
    exit 0
}

do_uninstall() {
    echo "=== uninstall $APP_NAME ==="
    
    echo "[STOP] $SERVICE_NAME"
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true
    
    echo "[DISABLE] $SERVICE_NAME"
    systemctl disable "$SERVICE_NAME" 2>/dev/null || true
    
    echo "[REMOVE] systemd service file"
    rm -f /etc/systemd/system/"$SERVICE_NAME".service
    systemctl daemon-reload || true
    
    echo "[REMOVE] $DEPLOY_DIR"
    rm -rf "$DEPLOY_DIR"
    
    echo "=== uninstall complete ==="
}

do_install() {
    echo "=== deploy $APP_NAME ==="

    if [ "$(id -u)" -ne 0 ]; then
        echo "[ERROR] must run as root"
        exit 1
    fi

    if [ ! -f "$BINARY" ]; then
        echo "[ERROR] binary not found: $BINARY"
        echo "        run scripts/build.sh first"
        exit 1
    fi

    echo "[STOP] existing service (if running)"
    systemctl stop "$SERVICE_NAME" 2>/dev/null || true

    # 清理旧 WebRTC 边车（mediamtx + fifo pusher）：直播已改回 fMP4/MSE 直供，
    # 不再需要外部 RTSP 中转。从旧版本升级时留着会白占 VPU/内存并抢 8554/8889 端口
    for u in mediamtx rk3588-edge-gateway-webrtc; do
        if [ -f /etc/systemd/system/"$u".service ]; then
            echo "[CLEAN] legacy webrtc unit: $u"
            systemctl stop "$u" 2>/dev/null || true
            systemctl disable "$u" 2>/dev/null || true
            rm -f /etc/systemd/system/"$u".service
        fi
    done

    echo "[INSTALL] $BINARY -> $DEPLOY_DIR/bin/"
    mkdir -p "$DEPLOY_DIR/bin"
    cp "$BINARY" "$DEPLOY_DIR/bin/$APP_NAME"
    chmod 755 "$DEPLOY_DIR/bin/$APP_NAME"

    echo "[INSTALL] config/ (先回拉设备端 config.txt，网页修改不丢失；再整目录覆盖)"
    if [ -f "$DEPLOY_DIR/config/config.txt" ]; then
        cp "$DEPLOY_DIR/config/config.txt" "$PROJECT_DIR/config/config.txt"
    fi
    rm -rf "$DEPLOY_DIR/config"
    cp -r "$PROJECT_DIR/config" "$DEPLOY_DIR/config"

    echo "[INSTALL] html/"
    rm -rf "$DEPLOY_DIR/html"
    cp -r "$PROJECT_DIR/html" "$DEPLOY_DIR/html"

    echo "[INSTALL] logs/"
    mkdir -p "$DEPLOY_DIR/logs"

    echo "[INSTALL] recordings/"
    mkdir -p "$DEPLOY_DIR/recordings"

    echo "[INSTALL] systemd service"
    cp "$SERVICE_SRC" /etc/systemd/system/"$SERVICE_NAME".service
    systemctl daemon-reload
    systemctl enable "$SERVICE_NAME"

    echo "[START] $SERVICE_NAME"
    systemctl start "$SERVICE_NAME"

    echo "=== deploy complete ==="

    echo ""
    systemctl status "$SERVICE_NAME" --no-pager -l
}

ACTION="${1:--i}"

case "$ACTION" in
    -i) do_install ;;
    -u) do_uninstall ;;
    -r) do_uninstall; do_install ;;
    -h|--help) usage ;;
    *)  echo "[ERROR] unknown option: $ACTION"; usage ;;
esac
