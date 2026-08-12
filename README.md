# data_transport_test

基于 epoll 的 **CAN 数据接收服务**，内置 Web 管理界面、视频流与 systemd 看门狗。目标平台为 Orange Pi 5 Max（RK3588）/ Linux，使用 C11 标准。

## 功能概览

- **CAN 数据接收**：支持多接口、经典 CAN 与 CAN FD（最大 64 字节），每接口独立接收过滤器。
- **日志记录为主**：接收的 CAN 帧写入日志，并通过 Web 管理界面支持运行配置与日志管理。
- **Web 管理界面（HTTP :80）**：系统监控仪表盘、CAN 状态/开关、运行配置热修改、日志管理、视频流、重启/关机。
- **视频流**：V4L2 MJPEG 采集，提供 `/video/mjpeg` 流式输出。
- **systemd 看门狗**：多槽位线程心跳监控，使用 `sd_notify` 进行就绪与喂狗。
- **日志轮转**：信息/错误分级日志，按天生成，单文件超过 10MB 自动轮转。
- **配置热更新**：Web 页面修改后写入 `config/config.txt`，运行时立即生效。

## 架构

```
                           ┌─────────────────────────────────────────────┐
 CAN bus ── can0 ────────► │  rx_task（epoll 读）                        │
 CAN bus ── can1 ───────► │   · 读取 CAN 帧（记录日志）               │
 CAN FD（≤64B）           │   · accept TCP 客户端（保留扩展）          │
                           │   · 读取 TCP 数据（当前丢弃）             │
                           └─────────────────────────────────────────────┘

 系统监控 / 配置 / 视频 ──► HTTP :80（Web 界面 + REST API + /video/mjpeg）
```

单进程多线程 + 双 epoll：

| 线程 | 职责 |
|---|---|
| `main` | 初始化、配置加载、退出编排 |
| `rx_task` | epoll 读：CAN 帧读取、TCP accept、TCP 数据读取 |
| `tx_task` | epoll 写：TCP 下行写入（预留） |
| `http_server_task` | HTTP :80 主循环与请求处理 |
| `video_stream_task` | V4L2 MJPEG 采集与帧发布 |
| `watchdog_task` | 线程心跳监控与 `sd_notify` |

## 目录结构

```
project1/
├── CMakeLists.txt
├── README.md
├── data_transport_test.service   # systemd 服务单元
├── bin/                          # 可执行输出
├── build/                        # CMake 构建目录
├── include/                      # 头文件
│   ├── core/
│   ├── can/
│   ├── net/
│   ├── http/
│   ├── video/
│   └── watchdog/
├── source/                       # 源码
│   ├── core/
│   ├── can/
│   ├── net/
│   ├── http/
│   ├── video/
│   └── watchdog/
├── html/                         # Web 前端静态资源
├── logs/                         # 运行日志
├── config/                       # 运行时配置
└── scripts/
    ├── build.sh
    ├── deploy.sh
    └── can_bench.sh
```

## 依赖与构建

依赖：gcc（C11）、cmake ≥ 3.10、libsystemd、libnl-3、libnl-route-3、Linux 内核 SocketCAN 支持。

```bash
apt install gcc cmake libsystemd-dev libnl-3-dev libnl-route-3-dev

./scripts/build.sh -R   # Release（默认）
./scripts/build.sh -D   # Debug
./scripts/build.sh -C   # 清理构建产物与 logs
```

构建产物：`bin/data_transport_test`

## 运行

```bash
cd /home/orangepi/project1
./bin/data_transport_test
```

> 请在项目根目录运行，确保 `config/`、`html/` 和 `logs/` 可用。

## 部署（systemd）

```bash
sudo ./scripts/deploy.sh        # 安装并启动
sudo ./scripts/deploy.sh -u     # 卸载
sudo ./scripts.deploy.sh -r     # 重装
```

默认部署目录为 `/opt/data_transport_test/`，服务名为 `data_transport_test`。

```bash
systemctl status data_transport_test
journalctl -u data_transport_test -f
```

部署要点：

- `Type=notify`
- `WatchdogSec=10`
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
- `ProtectSystem=strict`

## 配置

`config/config.txt` 由 Web 页面保存并在运行时生效。

| 键 | 格式 | 说明 |
|---|---|---|
| `can_ifname` | `<name>` | CAN 接口名称，可重复 |
| `can_bitrate` | `<name> <bps>` | 比特率，默认 500000 |
| `can_dbitrate` | `<name> <bps>` | FD 数据比特率，默认 2000000 |
| `can_fd` | `<name> on\|off` | CAN FD 开关 |
| `can_up` | `<name> on\|off` | 启动时 bring up |
| `tcp_port` | `<port>` | TCP 端口，默认 6666 |
| `max_clients` | `<n>` | 最大客户端数，默认 16 |
| `video_device` | `<path>` | 视频设备，默认 `/dev/video0` |
| `video_width` / `video_height` | `<n>` | 分辨率，默认 640×480 |
| `http_port` | `<port>` | HTTP 管理端口，默认 80 |

未提供配置文件时使用内置默认值。

## Web 管理界面（HTTP :80）

- `/`：仪表盘
- `/config`：运行配置
- `/logs`：日志管理

### REST API

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| GET | `/api/system` | 无 | 系统监控数据 |
| GET | `/api/can` | 无 | CAN 状态 |
| POST | `/api/can/toggle` | root | 切换 CAN 接口 |
| GET/POST | `/api/config` | root | 读取/写入配置 |
| GET | `/api/network` | 无 | 网络统计 |
| GET | `/api/video/devices` | 无 | 摄像头设备列表 |
| GET | `/api/video/caps` | 无 | 视频参数列表 |
| GET | `/video/mjpeg` | 无 | MJPEG 视频流 |
| GET | `/logs`、`/logfile/*` | root | 日志列表 / 下载 / 删除 |
| GET | `/api/reboot`、`/api/shutdown` | root | 重启 / 关机 |

HTTP 写操作接口要求 root 权限。

## TCP 数据通道

当前实现聚焦 CAN 帧接收与日志记录。TCP 端口与客户端连接管理保留为后续扩展。

## 看门狗

- `can`、`tcp`、`http`、`video`、`main` 五个槽位线程心跳监控
- 心跳超时触发整体退出
- 每 5 秒调用 `sd_notify("WATCHDOG=1")`

## 日志

按级别与日期归档，单文件超过 10MB 自动轮转。

```
logs/
├── data_transport_test_info_YYYYMMDD.log
└── data_transport_test_error_YYYYMMDD.log
```

## 清理说明

`./scripts/build.sh -C` 将删除：

- `build/`
- `bin/`
- `cmake/`
- CMake 生成文件（`CMakeCache.txt`、`CMakeFiles`、`cmake_install.cmake`、`Makefile`）
- 并重建空 `logs/`
