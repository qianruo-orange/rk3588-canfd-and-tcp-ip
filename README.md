# data_transport_test

基于 epoll 的 **CAN 数据接收服务**，内置 Web 管理界面、视频流与 systemd 看门狗，以 systemd 守护进程方式运行（`Type=notify`）。

> 目标平台：Orange Pi 5 Max（RK3588）/ Linux / C11

## 功能特性

- **CAN 数据接收**：多接口、经典 CAN / CAN FD（最大 64 字节），每接口独立接收过滤器
- **不做 TCP 数据转发**：CAN 帧仅接收并记录日志，不向 TCP 客户端推送；TCP→CAN 上行同样不实现（客户端上行数据被忽略）。TCP 端口与客户端连接管理保留为扩展预留
- 支持经典 CAN 与 CAN FD（最大 64 字节），多 CAN 接口，每接口独立接收过滤器
- **Web 管理界面（HTTP :80）**：系统监控仪表盘、CAN 状态/开关、运行配置热修改、日志管理、视频流、重启/关机
- **视频流**：V4L2 MJPEG 采集，`/video/mjpeg` 以 multipart MJPEG 推流
- **systemd 看门狗**：多槽位线程心跳监控 + `sd_notify` 喂狗
- 分级日志（INFO/ERROR），按天分文件 + 10MB 轮转备份
- 配置由 Web 界面写入 `config/config.txt`，启动时加载、修改后立即热生效

## 架构

```
                           ┌─────────────────────────────────────────────┐
 CAN bus ── can0 ────────► │  rx_task（epoll 读）                        │
 CAN bus ── can1 ────────► │   · 读取 CAN 帧（记录日志，不转发）         │
 CAN FD（≤64B）           │   · accept TCP 客户端（预留，不转发数据）    │
                           │   · 读取 TCP 数据（丢弃，不做 TCP→CAN）     │
                           └─────────────────────────────────────────────┘

 系统监控 / 配置 / 视频 ──► HTTP :80（Web 界面 + REST API + /video/mjpeg）
```

单一进程、多线程 + 双 epoll 引擎：

| 线程 | 职责 |
|---|---|
| `main` | 模块初始化 / 退出编排 |
| `rx_task` | epoll 读：CAN 帧读取、TCP 连接 accept、TCP 数据读取（读取后丢弃） |
| `tx_task` | epoll 写：向 TCP 客户端刷出下行数据（预留，当前无下行数据） |
| `http_server_task` | HTTP :80 主循环 + 每连接处理线程 |
| `video_stream_task` | V4L2 采集 MJPEG，发布最新帧 |
| `watchdog_task` | 各槽位心跳监控 + systemd `sd_notify` |

## 目录结构

```
project1/
├── CMakeLists.txt
├── README.md
├── data_transport_test.service   # systemd 服务单元
├── bin/                          # 构建产物（data_transport_test）
├── build/                        # CMake 构建目录
├── include/                      # 头文件
│   ├── core/                     # common / config / gateway / log
│   ├── can/                      # can_socket
│   ├── net/                      # tcp_server
│   ├── http/                     # http / http_internal
│   ├── video/                    # video_stream
│   └── watchdog/                 # watchdog
├── source/
│   ├── core/                     # main / config / gateway / log / receive_data / send_data
│   ├── can/                      # can_socket
│   ├── net/                      # tcp_server
│   ├── http/                     # http + API（system / can / config / network / video / logs / reboot）
│   ├── video/                    # video_stream
│   └── watchdog/                 # watchdog
├── html/                         # Web 前端（index / config / 日志页 + css/js）
├── logs/                         # 运行日志
├── config/                       # 配置文件目录（部署时创建，仓库默认缺失）
└── scripts/
    ├── build.sh                  # 构建
    ├── deploy.sh                 # 部署 systemd 服务
    └── can_bench.sh              # CAN 极限速率测试
```

## 依赖与构建

依赖：gcc（C11）、cmake ≥ 3.10、libsystemd、libnl-3、libnl-route-3、Linux 内核 SocketCAN 支持。

```bash
apt install gcc cmake libsystemd-dev libnl-3-dev libnl-route-3-dev

./scripts/build.sh -R   # Release（默认，-O3）
./scripts/build.sh -D   # Debug（-O0 -g）
./scripts/build.sh -C   # 清理构建目录
```

产物：`bin/data_transport_test`

## 运行

```bash
# 直接前台运行（需在包含 config/、html/、logs/ 的工作目录下）
./bin/data_transport_test
# Ctrl+C 停止
```

> ⚠️ `config/config.txt`、`html/`、`logs/` 均为**相对路径**，需在工程根目录（或部署目录）下运行；仓库已附带示例 `config/config.txt`（无此文件时使用内置默认配置）。

## 部署（systemd）

```bash
sudo ./scripts/deploy.sh        # 安装并启动（默认）
sudo ./scripts/deploy.sh -u     # 卸载
sudo ./scripts/deploy.sh -r     # 重装
```

部署位置 `/opt/data_transport_test/`（`bin/`、`config/`、`html/`、`logs/`），服务名 `data_transport_test`：

```bash
systemctl status data_transport_test
journalctl -u data_transport_test -f
```

服务单元要点：

- `Type=notify` + `WatchdogSec=10`（进程通过 `sd_notify` 就绪与喂狗）
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`（配置/收发 CAN）
- `ProtectSystem=strict`，仅 `logs/`、`config/` 可写，`html/` 只读

## 配置

配置文件 `config/config.txt`，由 Web 配置页写入（`config_save()` 自动保存），启动时 `config_load()` 加载；修改后立即热生效。

| 键 | 格式 | 说明 |
|---|---|---|
| `can_ifname` | `<name>` | 声明 CAN 接口（可重复） |
| `can_bitrate` | `<name> <bps>` | 比特率（默认 500000） |
| `can_dbitrate` | `<name> <bps>` | FD 数据比特率（默认 2000000） |
| `can_fd` | `<name> on\|off` | 使能 CAN FD |
| `can_up` | `<name> on\|off` | 启动时 bring up |
| `tcp_port` | `<port>` | 下行 TCP 端口（默认 6666） |
| `max_clients` | `<n>` | 最大客户端数（默认 16） |
| `video_device` | `<path>` | 视频设备（默认 `/dev/video0`） |
| `video_width` / `video_height` | `<n>` | 采集分辨率（默认 640×480） |
| `http_port` | `<port>` | Web 管理端口（默认 80） |

无配置文件时使用内置默认值（`can0`/`can1`，500 kbps / FD 2 Mbps）。

## Web 管理界面（HTTP :80）

- `/` 仪表盘：CPU / 逐核 / NPU / GPU / 内存 / 磁盘 / 温度 / 网络速率 + 视频流
- `/config` 配置页：CAN 参数、过滤器、TCP 端口、视频参数（需 root 认证）
- `/logs` 日志页：列表 / 下载 / 删除 / 打包 tar.gz（需 root 认证）

### REST API

| 方法 | 路径 | 认证 | 说明 |
|---|---|---|---|
| GET | `/api/system` | 无 | 系统监控（CPU/NPU/GPU/内存/磁盘/温度） |
| GET | `/api/can` | 无 | CAN 接口状态（up/bitrate/fd/dbitrate） |
| POST | `/api/can/toggle` | root | 开关 CAN 接口 |
| GET/POST | `/api/config` | root | 读 / 写运行配置（热生效） |
| GET | `/api/network` | 无 | eth0 / wlan0 流量统计 |
| GET | `/api/video/devices` | 无 | 摄像头设备枚举 |
| GET | `/api/video/caps` | 无 | 预设分辨率列表（非真实枚举，避免驱动卡死） |
| GET | `/video/mjpeg` | 无 | MJPEG 视频流 |
| GET | `/logs`、`/logfile/*` | root | 日志列表 / 下载 / 删除 |
| GET | `/api/reboot`、`/api/shutdown` | root | 系统重启 / 关机 |

认证方式：HTTP Basic，校验系统账号密码哈希；root 接口要求 `uid == 0`。

## TCP 数据通道

> 本服务**不实现 TCP 数据转发**：CAN 帧仅接收并记录日志，不向 TCP 客户端推送；TCP→CAN 上行同样不实现（客户端上行数据被忽略）。TCP 端口与客户端连接管理、`tcp_task` 刷出机制保留为扩展预留。

## 看门狗

- 槽位与各工作线程一一对应：`can`(3s×3)、`tcp`(5s×3)、`http`(5s×3)、`video`(5s×3)、`main`(15s×1，主循环)
- 某槽位心跳超时达上限 → 记录错误并触发整体退出
- `WATCHDOG_USEC`（systemd `WatchdogSec=10`）→ 每 5s `sd_notify("WATCHDOG=1")`

## 日志

按级别 + 日期分文件，单文件超 10MB 轮转为 `.1`：

```
logs/
├── data_transport_test_info_20260808.log
└── data_transport_test_error_20260808.log
```

## 已知问题 / 待办

> 代码扫描发现的问题已全部修复（见下方「已修复」）；本节保留设计决策项与低风险遗留项。

### 设计决策（非缺陷）

1. **不做 TCP 数据转发**：CAN 帧仅接收并记录日志，不向 TCP 客户端推送；TCP→CAN 上行同样不实现（客户端上行数据被忽略）。TCP 端口与客户端连接管理、`tcp_task` 刷出机制保留为扩展预留。
2. **监控/视频 API 匿名访问（安全取舍）**：`/api/system`、`/api/can`、`/api/network`、`/api/video/*`、`/video/mjpeg` 无认证——Web 前端（仪表盘、视频流）依赖免凭证访问，强制认证会破坏这些页面；所有**写操作**接口（配置、CAN 开关、重启/关机、日志管理）均要求 root 认证。如需收紧，可改为 Basic 认证并让前端携带凭据。
3. **watchdog `_exit(1)` 快速兜底（已优化）**：检测到线程 STUCK 后，先置 `running=0` 通知各线程停止、`sync()` 同步磁盘，并留 200ms 清理窗口（日志落盘 / 视频设备关闭），随后强制 `_exit(1)` 由 systemd `Restart=on-failure` 重启——既能快速兜底卡死线程，也尽量完成可执行的清理。
4. **HTTP 单线程 epoll 架构（已优化）**：所有 HTTP 请求由 `http_server_task` 单线程串行处理，避免多线程连接管理复杂度；重活（日志打包 / 配置 / 大文件）已通过体积上限（打包 100MB、静态文件 20MB）、`WD_HTTP` 持续喂狗、认证暴力破解熔断（10s/20 次）等机制控制最坏情况，正常路径不阻塞。

### 已修复（21 项）

- **SIGPIPE**：`signal(SIGPIPE, SIG_IGN)`，向已断开 socket 写入不再终止进程
- **CAN 断线重连**：重连后重新 `epoll_ctl(ADD)` 新 fd
- **`can_socket_configure`**：改用 `rtnl_link_get_kernel()` 探测存在性，消除启动时 "Object busy" 失败
- **tx_task 忙循环**：`wlen==0` 时从写 epoll 移除 fd，不再空转
- **视频流重启**：worker 单线程 + `restart_req` 原子标志，消除 sleep 代替 join 与双重 deinit
- **`flush_client` 阻塞**：client fd 非阻塞，`write_all` 遇 EAGAIN 立即返回
- **HTTP 并发**：活跃连接上限 64，超出拒绝
- **HTTP 连接计数**：`client_handler` 空读与 epoll 的 EPOLLHUP 路径均正确归还连接计数（曾有两处泄漏，100 次连接回归测试通过）
- **认证线程安全**：`getpwnam/getspnam/crypt` 整体加锁
- **`max_clients` 越界**：读写两侧 clamp 到 `TCP_MAX_CLIENTS`
- **`http_can_status`**：JSON 组装加边界检查（安全截断）
- **`video_stream_wait_next`**：分配失败返回空帧，推流侧跳过，避免 `write(NULL)`
- **日志轮转**：每次写入前检查大小，同一天内超 10MB 即轮转
- **HTTP fd 非阻塞**：accept 后置 `O_NONBLOCK`
- **配置**：仓库已附带 `config/config.txt`；新增 `http_port` 配置键
- **`serve_log_pack`**：先 `chdir` 到日志目录再打包
- **Web 配置接口数**：后端支持全部 `CAN_MAX_IFACES` 接口
- **遗留二进制**：删除 `bin/socketapp`、`bin/socketcan`
- **线程退出竞态**：detached 线程经包装器递减活跃计数，主线程退出时有界等待线程结束后再执行 dtor，视频设备由 worker 干净关闭（VIDIOC_STREAMOFF）
- **CAN 重连竞态**：`handle_can_input()` 读 + 重连整体纳入 `can_mutex`，与 HTTP 配置热更新彻底互斥
- **TCP 端口热更新**：配置端口变更加 `client_mutex` 串行化，并检查 `epoll_ctl` 返回值
- **`http_can_toggle`**：检查 `rtnl_link_change` 返回值，失败返回 500
- **`serve_log_pack` 阻塞**：打包体积上限 100MB 并持续喂狗，防止长时间阻塞 HTTP / 看门狗误杀
- **慢请求喂狗**：CAN 配置循环持续喂 `WD_HTTP`，避免配置耗时触发看门狗重启
- **认证暴力破解熔断**：10 秒窗口内失败 20 次后直接拒绝（不执行 crypt），防止 CPU DoS
- **POST body 截断**：body 超出缓冲区返回 413 拒绝，避免配置被静默截断
- **静态文件上限**：响应文件超过 20MB 返回 413
- **`http_can_toggle`**：URL 解码 + 接口名/action 校验 + `can_mutex` 串行化 + netlink 判空

### 低风险遗留项（暂不处理）

- **watchdog 卡死线程**：检测到线程 STUCK 后置运行标志，但卡死的线程不会退出；主线程有界等待后仍执行清理（存在竞态可能），最终由 systemd `WatchdogSec` 兜底杀进程重启。
- **`can_socket_configure` 命令拼接**：`cmd[512]` + APPEND 已做边界检查，接口名极长且参数全开时才可能触发截断导致命令不完整（当前参数不会触发）。

