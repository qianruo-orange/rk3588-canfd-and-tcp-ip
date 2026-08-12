#!/bin/bash
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DEPLOY_DIR="/opt/data_transport_test"
BINARY="$PROJECT_DIR/bin/data_transport_test"
SERVICE_SRC="$PROJECT_DIR/data_transport_test.service"
SERVICE_NAME="data_transport_test"

usage() {
    echo "Usage: $0 [-i | -u | -r]"
    echo "  -i   Install: deploy and start service (default)"
    echo "  -u   Uninstall: stop service, remove files"
    echo "  -r   Reinstall: uninstall then install"
    echo "  -h   Show this help"
    exit 0
}

do_uninstall() {
    echo "=== uninstall data_transport_test ==="
    
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
    echo "=== deploy data_transport_test ==="

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

    echo "[INSTALL] $BINARY -> $DEPLOY_DIR/bin/"
    mkdir -p "$DEPLOY_DIR/bin"
    cp "$BINARY" "$DEPLOY_DIR/bin/data_transport_test"
    chmod 755 "$DEPLOY_DIR/bin/data_transport_test"

    echo "[INSTALL] config/"
    mkdir -p "$DEPLOY_DIR/config"
    rm -f "$DEPLOY_DIR/config/config.txt"
    echo "  (using defaults — configure via web UI)"

    echo "[INSTALL] html/"
    rm -rf "$DEPLOY_DIR/html"
    cp -r "$PROJECT_DIR/html" "$DEPLOY_DIR/html"

    echo "[INSTALL] logs/"
    mkdir -p "$DEPLOY_DIR/logs"

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
