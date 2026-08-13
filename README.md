# rk3588-canfd-and-tcp-ip-communication

[![Platform](https://img.shields.io/badge/Platform-RK3588-orange.svg)](https://github.com/qianruo-orange/rk3588-canfd-and-tcp-ip-communication)
[![Language](https://img.shields.io/badge/Language-C11-blue.svg)](https://github.com/qianruo-orange/rk3588-canfd-and-tcp-ip-communication)
[![OS](https://img.shields.io/badge/OS-Linux-green.svg)](https://github.com/qianruo-orange/rk3588-canfd-and-tcp-ip-communication)

基于 epoll 的 CAN 数据接收与 TCP/IP 通信解决方案，内置 Web 管理界面、视频流与 systemd 看门狗。该项目适用于 Orange Pi 5 Max（RK3588）/ Linux 运行环境，使用 C11 实现。

## 项目简介

本项目面向嵌入式工业场景与边缘网关场景，整合了以下核心能力：

- CAN 总线数据采集与日志记录
- CAN FD 兼容支持
- TCP/IP 通信与连接管理
- Web 管理界面配置与监控
- 视频流服务
- 运行时系统守护与 watchdog 监控

## 快速开始

```bash
cd /home/orangepi/project1
./scripts/build.sh -R
./bin/rk3588-canfd-and-tcp-ip-communication
```

## 项目亮点

- 面向 RK3588 平台设计，适合工业控制与边缘设备部署
- 同时覆盖 CAN、CAN FD、TCP、HTTP 与视频流功能
- Web 端可查看状态、更新配置、管理日志和重启设备
- 采用 systemd watchdog 提升稳定性与故障恢复能力
- 支持日志按日期和大小自动轮转，便于长期运行追踪

## 功能特点

- CAN 数据接收：支持经典 CAN 与 CAN FD，多接口管理
- 日志记录：接收的 CAN 帧和系统事件按级别写入日志
- Web 管理界面：提供数据监控、配置修改、日志查看与重启控制
- 视频流服务：通过 V4L2 接入摄像头并输出 MJPEG 视频流
- systemd 看门狗：监控关键线程心跳，防止异常卡死
- 热更新配置：Web 页面修改后可立即生效
- 运行维护：日志轮转、清理脚本、部署脚本

## 系统架构

```text
CAN bus ──► rx_task ──► 日志记录 / TCP 处理 / 配置更新
                 │
                 ├──► HTTP Server ──► Web 管理界面
                 │
                 ├──► video_stream_task ──► MJPEG 视频流
                 │
                 └──► watchdog_task ──► systemd watchdog / 线程监控
```

单进程多线程架构如下：

| 线程 | 职责 |
| --- | --- |
| `main` | 初始化、配置加载、退出编排 |
| `rx_task` | epoll 读：CAN 帧读取、TCP accept、TCP 数据读取 |
| `tx_task` | epoll 写：TCP 下行写入（预留） |
| `http_server_task` | HTTP :80 主循环与请求处理 |
| `video_stream_task` | V4L2 MJPEG 采集与帧发布 |
| `watchdog_task` | 线程心跳监控与 `sd_notify` |

## 目录结构

```text
rk3588-canfd-and-tcp-ip-communication/
├── CMakeLists.txt
├── README.md
├── rk3588-canfd-and-tcp-ip-communication.service      # systemd 服务单元
├── bin/                            # 可执行输出
├── build/                          # CMake 构建目录
├── include/                        # 头文件
│   ├── core/
│   ├── can/
│   ├── net/
│   ├── http/
│   ├── video/
│   └── watchdog/
├── source/                         # 源代码
│   ├── core/
│   ├── can/
│   ├── net/
│   ├── http/
│   ├── video/
│   └── watchdog/
├── html/                           # Web 前端静态资源
├── logs/                           # 运行日志
├── config/                         # 运行时配置
├── scripts/
│   ├── build.sh
│   └── deploy.sh
├── data/
├── .gitignore
└── LICENSE
```

## 依赖要求

该项目依赖以下组件：

- gcc（C11）
- cmake ≥ 3.10
- libsystemd
- libnl-3
- libnl-route-3
- Linux SocketCAN 支持

安装示例：

```bash
apt install gcc cmake libsystemd-dev libnl-3-dev libnl-route-3-dev
```

## 构建与版本信息

```bash
cd /home/orangepi/project1
./scripts/build.sh -R   # Release（默认）
./scripts/build.sh -D   # Debug
./scripts/build.sh -C   # 清理构建产物与 logs
```

当前版本信息由 CMake 统一管理，工程版本号和构建信息会在配置阶段生成到头文件：

```text
include/core/version.h
```

生成的宏包括：

- `APP_NAME`
- `APP_VERSION`
- `APP_GIT_COMMIT`
- `APP_GIT_BRANCH`
- `APP_GIT_DIRTY`
- `APP_BUILD_TYPE`
- `APP_BUILD_DATE`

构建脚本会在编译完成后输出当前版本与构建模式，例如：

```text
[BUILD] version=1.0.0 build_type=Release
```

构建产物：

```bash
./bin/rk3588-canfd-and-tcp-ip-communication
```

## 运行方式

```bash
cd /home/orangepi/project1
./bin/rk3588-canfd-and-tcp-ip-communication
```

> 请在项目根目录运行，确保 `config/`、`html/` 和 `logs/` 目录可用。

## systemd 部署

```bash
sudo ./scripts/deploy.sh        # 安装并启动
sudo ./scripts/deploy.sh -u     # 卸载
sudo ./scripts/deploy.sh -r     # 重装
```

默认安装路径：`/opt/rk3588-canfd-and-tcp-ip-communication/`

服务名称：`rk3588-canfd-and-tcp-ip-communication`

查看服务状态：

```bash
systemctl status rk3588-canfd-and-tcp-ip-communication
journalctl -u rk3588-canfd-and-tcp-ip-communication -f
```

部署要点：

- `Type=notify`
- `WatchdogSec=10`
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
- `ProtectSystem=strict`

## 配置说明

`config/config.txt` 由 Web 页面保存，并在运行时生效。

| 键 | 格式 | 说明 |
| --- | --- | --- |
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

未提供配置文件时，将使用内置默认值。

## Web 管理界面

服务默认监听 HTTP 端口 80，提供以下入口：

- `/`：仪表盘
- `/config`：运行配置
- `/logs`：日志管理

### REST API

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
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

当前实现聚焦 CAN 帧接收与日志记录，TCP 端口与客户端连接管理保留为后续扩展。

## 看门狗机制

- `can`、`tcp`、`http`、`video`、`main` 五个关键线程进行心跳监控
- 心跳超时将触发整体退出
- 每 5 秒调用 `sd_notify("WATCHDOG=1")`

## 日志管理

日志按级别与日期归档，单文件超过 10MB 自动轮转。

```text
logs/
├── rk3588-canfd-and-tcp-ip-communication_info_YYYYMMDD.log
└── rk3588-canfd-and-tcp-ip-communication_error_YYYYMMDD.log
```

## 清理说明

执行 `./scripts/build.sh -C` 时，将删除以下内容：

- `build/`
- `bin/`
- `include/core/version.h`（由 CMake 生成的版本头）
- CMake 生成文件（`CMakeCache.txt`、`CMakeFiles`、`cmake_install.cmake`、`Makefile`）
- 并重建空 `logs/`

## 预览截图

> 这里可以放项目运行界面截图、Web 页面截图或设备连接示意图。

![Web UI Preview](https://via.placeholder.com/1200x600.png?text=Web+UI+Preview)

## 项目亮点

- 面向 RK3588 平台开发，适合工业控制和边缘网关场景
- 同时覆盖 CAN / CAN FD / TCP / HTTP / 视频流等功能
- 通过 Web 页面快速查看设备状态与配置参数
- 采用 systemd 看门狗机制增强稳定性与可靠性
- 支持日志轮转、热更新与运行时管理

## 许可证

该项目采用适用于本仓库的开源许可证，具体内容请参考仓库中的许可证文件。

## 联系方式

如需二次开发、部署咨询或定制化扩展，可直接基于当前仓库进行修改与定制。