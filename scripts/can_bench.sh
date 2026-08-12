#!/bin/bash
# CAN极限速率测试脚本
set -e

echo "=== 重置CAN接口 ==="
sudo ip link set can0 down 2>/dev/null || true
sudo ip link set can1 down 2>/dev/null || true
sudo ip link set can0 up type can bitrate 500000 dbitrate 2000000 fd on
sudo ip link set can1 up type can bitrate 500000 dbitrate 2000000 fd on
sudo ip link set can0 txqueuelen 1000
sudo ip link set can1 txqueuelen 1000
sleep 0.3

echo ""
echo "=== CAN FD 64字节极限 (2Mbps数据速率) ==="

for GAP in 1.0 0.5 0.2 0.1; do
    echo -n "  gap=${GAP}ms: "
    # 后台接收
    candump -L can1 > /tmp/can_rx.log 2>/dev/null &
    PID=$!
    sleep 0.3
    # 发送200帧
    cangen can0 -g $GAP -I 123 -L 64 -n 200 2>&1 | grep -c "write:" || true
    sleep 1.5
    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    RX=$(wc -l < /tmp/can_rx.log)
    LOSS=$((100 - RX*100/200))
    echo "收${RX}/200  丢包率${LOSS}%"
    rm -f /tmp/can_rx.log
done

echo ""
echo "=== 标准CAN 8字节极限 (500kbps) ==="

for GAP in 0.5 0.2 0.1 0.05; do
    echo -n "  gap=${GAP}ms: "
    candump -L can1 > /tmp/can_rx.log 2>/dev/null &
    PID=$!
    sleep 0.3
    cangen can0 -g $GAP -I 456 -L 8 -n 200 2>&1 | grep -c "write:" || true
    sleep 1.5
    kill $PID 2>/dev/null || true
    wait $PID 2>/dev/null || true
    RX=$(wc -l < /tmp/can_rx.log)
    LOSS=$((100 - RX*100/200))
    echo "收${RX}/200  丢包率${LOSS}%"
    rm -f /tmp/can_rx.log
done

echo ""
echo "=== DONE ==="
