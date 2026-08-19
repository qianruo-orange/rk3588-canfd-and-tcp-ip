# rk3588-canfd-and-tcp-ip-communication

基于 epoll 的 CAN / CAN FD 数据采集、DBC 信号解析与 TCP/IP 通信解决方案，内置 Web 管理界面、视频流与 systemd 看门狗，支持按系统信息自动识别可配置 CAN 通道。适用于 RK3588（如 Orange Pi 5 Max）/ Linux 运行环境，使用 C11 实现。

## 功能特性

- CAN 数据采集：支持经典 CAN 与 CAN FD，多接口管理，自动识别系统 CAN 接口
- DBC 信号解析：解析 DBC 数据库，将 CAN 帧解码为物理量信号（接收 / 发送双方向）
- TCP/IP 通信：TCP 服务监听，端口与绑定网卡均可配置
- Web 管理界面：数据监控、配置修改、日志查看与重启 / 关机控制
- 视频流服务：通过 V4L2 接入摄像头并输出 MJPEG 视频流
- systemd 看门狗：监控关键线程心跳，防止异常卡死
- 热更新配置：Web 页面修改后立即生效
- 运行维护：日志按日期与大小自动轮转、清理脚本、部署脚本

## 系统架构

单进程多线程架构，所有线程由 `source/core/main.c` 的模块总表 `g_modules[]` 统一注册并拉起：

```text
        ┌─► can_recv_task ──► SocketCAN 收帧 ──► 入 rxq（原始帧）+ 接收方向 DBC 解码
 main ──┼─► can_send_task ──► 排空 txq（原始帧）写 socket + 发送方向 DBC 解码
        ├─► tcp_task      ──► TCP 监听 / 收发
        ├─► http_task     ──► Web 管理 + REST API + MJPEG 推流
        ├─► video_task    ──► V4L2 采集
        └─► watchdog_task ──► 心跳监控 + sd_notify
```

CAN 数据流（队列中均为原始帧，DBC 解析结果单独供前端展示）：

```text
接收：CAN 帧 ─► can_recv_task ─► 入 rxq（原始帧）/ 接收方向 DBC 解码 ─► /api/can/decoded
发送：业务线程压入 txq（原始帧） ─► can_send_task 写 socket ─► 发送方向 DBC 解码 ─► /api/can/decoded/tx
```

| 线程 | 职责 |
| --- | --- |
| `main` | 初始化、配置加载、DBC 加载、退出编排、喂狗 |
| `can_recv` | CAN 帧接收、入 rxq（原始帧）、接收方向 DBC 解码 |
| `can_send` | 排空 txq（原始帧）写 socket、发送方向 DBC 解码 |
| `tcp` | TCP 监听、连接管理与数据收发 |
| `http` | HTTP 管理服务主循环、REST API、MJPEG 推流 |
| `video` | V4L2 MJPEG 采集与帧发布 |
| `watchdog` | 线程心跳监控与 `sd_notify` |

> 注：`signal` 模块仅注册信号处理（`init`），`task` 为空，不创建线程。

## 目录结构

```text
rk3588-canfd-and-tcp-ip-communication/
├── CMakeLists.txt
├── README.md
├── rk3588-canfd-and-tcp-ip-communication.service  # systemd 服务单元
├── include/                          # 头文件
│   ├── core/
│   │   ├── common.h                  # 应用上下文与公共工具
│   │   ├── config.h                  # 配置结构
│   │   ├── log.h                     # 日志接口
│   │   └── version.h                 # 版本宏（CMake 生成）
│   ├── can/
│   │   ├── can_queue.h               # CAN 收发队列
│   │   ├── can_socket.h              # SocketCAN 接口
│   │   └── dbc_parser.h              # DBC 解析接口
│   ├── tcp/
│   │   └── tcp_server.h              # TCP 服务接口
│   ├── http/
│   │   ├── http.h                    # HTTP 服务接口
│   │   └── http_internal.h           # HTTP 内部定义
│   ├── video/
│   │   └── video_stream.h            # V4L2 视频流接口
│   └── watchdog/
│       └── watchdog.h                # 看门狗接口
├── source/                           # 源代码
│   ├── core/
│   │   ├── main.c                    # 入口、模块总表、线程编排
│   │   ├── config.c                  # 配置读写
│   │   └── log.c                     # 日志实现
│   ├── can/
│   │   ├── can_queue.c               # CAN 收发队列实现
│   │   ├── can_socket.c              # SocketCAN 收发
│   │   └── dbc_parser.c              # DBC 解析实现
│   ├── tcp/
│   │   └── tcp_server.c              # TCP 服务实现
│   ├── http/
│   │   ├── http.c                    # HTTP 路由与静态文件
│   │   ├── http_api_can.c            # CAN 相关 API
│   │   ├── http_api_dbc.c            # DBC 解码结果 API
│   │   ├── http_api_config.c         # 配置 API
│   │   ├── http_api_network.c        # 网络 API
│   │   ├── http_api_system.c         # 系统 API
│   │   ├── http_api_video.c          # 视频 API
│   │   ├── http_logs.c               # 日志查看
│   │   └── http_reboot.c             # 重启 / 关机
│   ├── video/
│   │   └── video_stream.c            # V4L2 视频流实现
│   └── watchdog/
│       └── watchdog.c                # 看门狗实现
├── html/                             # Web 前端
│   ├── index.html                    # 监控页
│   ├── config.html                   # 配置页
│   ├── dbc.html                      # DBC 解析页
│   ├── logs.html                     # 日志页
│   ├── css/
│   │   ├── common.css                # 全局共享样式
│   │   ├── config.css                # 配置页样式
│   │   ├── logs.css                  # 日志页样式
│   │   └── monitor.css               # 监控页样式
│   └── js/
│       ├── config.js                 # 配置页脚本
│       ├── dbc.js                    # DBC 页脚本
│       ├── logs.js                   # 日志页脚本
│       └── monitor.js                # 监控页脚本
├── config/                           # 运行时配置
│   ├── config.txt                    # 主配置
│   ├── can0.dbc                      # CAN0 通道 DBC 模板
│   └── can1.dbc                      # CAN1 通道 DBC 模板
├── docs/                             # 文档与截图
├── scripts/
│   ├── build.sh                      # 构建脚本
│   └── deploy.sh                     # 部署脚本
├── bin/                              # 可执行输出（构建生成）
├── build/                            # CMake 构建目录（构建生成）
├── logs/                             # 运行日志
└── .gitignore
```

## 依赖要求

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

## 构建

```bash
cd /home/orangepi/project1
./scripts/build.sh -R   # Release（默认）
./scripts/build.sh -D   # Debug
./scripts/build.sh -C   # 清理构建产物与 logs
```

构建产物：`./bin/rk3588-canfd-and-tcp-ip-communication`

版本信息由 CMake 在配置阶段生成到 `include/core/version.h`，包括 `APP_NAME`、`APP_VERSION`、`APP_GIT_COMMIT`、`APP_GIT_BRANCH`、`APP_GIT_DIRTY`、`APP_BUILD_TYPE`、`APP_BUILD_DATE`。

## 运行方式

```bash
cd /home/orangepi/project1
./bin/rk3588-canfd-and-tcp-ip-communication
```

> 请在项目根目录运行，确保 `config/`、`html/` 和 `logs/` 目录可用。

## systemd 部署

前置条件：已通过 `./scripts/build.sh -R` 生成可执行文件。

```bash
sudo ./scripts/deploy.sh        # 安装并启动（默认动作，等同 -i）
sudo ./scripts/deploy.sh -i     # 安装并启动
sudo ./scripts/deploy.sh -u     # 卸载（停止服务、删除 systemd 文件与安装目录）
sudo ./scripts/deploy.sh -r     # 重装（卸载后重新安装）
sudo ./scripts/deploy.sh -h     # 查看帮助
```

`deploy.sh` 需 root 权限，安装目录为 `/opt/rk3588-canfd-and-tcp-ip-communication/`，部署内容如下：

| 源 | 目标 |
| --- | --- |
| `bin/<可执行文件>` | `/opt/.../bin/` |
| `html/` | `/opt/.../html/` |
| systemd 服务单元 | `/etc/systemd/system/` |

> 注：`config/config.txt` 与 `config/` 下的 DBC 模板文件**不会**被复制；首次部署使用程序内置默认值，后续通过 Web 配置页保存配置、上传 DBC 文件。

服务名称：`rk3588-canfd-and-tcp-ip-communication`

查看服务状态：

```bash
systemctl status rk3588-canfd-and-tcp-ip-communication
journalctl -u rk3588-canfd-and-tcp-ip-communication -f
```

部署要点：

- `Type=notify`
- `WatchdogSec=10`
- `Restart=on-failure`（失败自动重启，间隔 5s）
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
- `ProtectSystem=strict`（文件系统只读，仅 `config/`、`logs/` 可写，`html/` 只读）

## 配置说明

`config/config.txt` 由 Web 页面保存，并在运行时生效。

| 键 | 格式 | 说明 |
| --- | --- | --- |
| `can_ifname` | `<name>` | CAN 接口名称，可重复；未提供时自动从系统枚举实际存在的 CAN 接口 |
| `can_bitrate` | `<name> <bps>` | 比特率，默认 500000 |
| `can_dbitrate` | `<name> <bps>` | FD 数据比特率，默认 2000000 |
| `can_fd` | `<name> on\|off` | CAN FD 开关 |
| `can_up` | `<name> on\|off` | 启动时 bring up |
| `tcp_port` | `<port>` | TCP 端口，默认 6666 |
| `max_clients` | `<n>` | 最大客户端数，默认 16 |
| `tcp_bind` | `<ifname>` | TCP 监听绑定网卡名，留空表示绑定所有网卡（`INADDR_ANY`） |
| `video_device` | `<path>` | 视频设备，默认 `/dev/video0` |
| `video_width` / `video_height` | `<n>` | 分辨率，默认 640×480 |
| `can_dbc` | `<name> <path>` | 按 CAN 通道配置 DBC 数据库文件路径，留空则不启用该通道信号解码 |

未提供配置文件时，将从系统枚举实际存在的 CAN 接口（netlink 路由，`kind=="can"`）并套用默认参数；若系统无法枚举到接口，则回退到 `can0` / `can1`。

### DBC 模板

`config/` 下提供两个 CAN 通道的 DBC 模板文件，可通过 `can_dbc <通道名> <路径>` 启用（或在 Web 配置页上传）：

- `config/can0.dbc`：动力 / 电池类（`EngineData`、`BatteryStatus`）
- `config/can1.dbc`：底盘 / 位置类（`Status` 含 Motorola 大端信号、`VehiclePos` 扩展帧）

> 解析器只识别 `BO_`（报文）与 `SG_`（信号）两类行，其余（含 `//` 注释）均忽略。ID 为十进制；扩展帧需在 29 位 ID 上置 bit31（如 `0x18FF50E5` → `2566869221`）。

## Web 管理界面

服务默认监听 HTTP 端口 80，提供以下入口：

- `/`：仪表盘
- `/dbc`：DBC 信号解析
- `/config`：运行配置
- `/logs`：日志管理

### REST API

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | `/api/system` | 无 | 系统监控数据 |
| GET | `/api/can` | 无 | CAN 接口状态 |
| GET | `/api/can/decoded` | 无 | 接收方向 DBC 解析后的最近 CAN 信号（JSON） |
| GET | `/api/can/decoded/tx` | 无 | 发送方向 DBC 解析后的最近 CAN 信号（JSON） |
| GET | `/api/can/frames` | 无 | 最近接收的原始 CAN 帧 |
| POST | `/api/can/send` | root | 发送 CAN 帧 |
| POST | `/api/can/dbc?ifname=` | root | 上传某通道的 DBC 文件 |
| POST | `/api/can/toggle` | root | 切换 CAN 接口开关 |
| GET/POST | `/api/config` | root | 读取 / 写入配置 |
| GET | `/api/network` | 无 | 网络统计 |
| GET | `/api/video/devices` | 无 | 摄像头设备列表 |
| GET | `/api/video/caps` | 无 | 视频参数列表 |
| GET | `/video/mjpeg` | 无 | MJPEG 视频流 |
| GET | `/api/logs`、`/logs`、`/logfile/*` | root | 日志列表 / 下载 / 删除 |

> 监控页与 DBC 页需要普通用户认证；配置页与写操作接口要求 root 权限。

## 看门狗机制

- 通过模块总表 `g_modules[]` 为 `can_recv`、`can_send`、`tcp`、`http`、`video`、`main` 注册心跳监控
- 每个模块带 `timeout` / `max_miss` 参数；`watchdog` 线程自身 `timeout=0`，不监督自己
- 线程以名字注册 / 喂狗 / 注销，超时日志可直接定位到具体线程名（如 `thread 'http'`）
- 心跳超时将触发整体退出
- 每 5 秒调用 `sd_notify("WATCHDOG=1")`（`WatchdogSec=10` 的一半）

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

## 许可证

该项目采用适用于本仓库的开源许可证，具体内容请参考仓库中的许可证文件。
