# rk3588-canfd-and-tcp-ip

基于 epoll 的 CAN / CAN FD 数据采集、DBC 信号解析与 TCP/IP 通信解决方案，内置 Web 管理界面、视频流、AI 目标检测（RKNN + YOLO26）、网络录像与 systemd 看门狗，支持按系统信息自动识别可配置 CAN 通道。适用于 RK3588（如 Orange Pi 5 Max）/ Linux 运行环境，使用 C11 实现。

## 功能特性

- CAN 数据采集：支持经典 CAN 与 CAN FD，多接口管理，自动识别系统 CAN 接口
- DBC 信号解析：解析 DBC 数据库，将 CAN 帧解码为物理量信号（接收 / 发送双方向）
- TCP/IP 通信：TCP 服务监听，端口与绑定网卡均可配置
- Web 管理界面：数据监控、配置修改、日志查看与重启 / 关机控制
- 视频流服务：通过 V4L2 接入摄像头并输出 MJPEG 视频流
- AI 目标检测：RKNN + YOLO26（官方 ultralytics rknn 单输出格式），多线程推理池并行加速，检测结果实时画框推流
- 网络录像：默认自动开启录屏（按天分目录 `recordings/YYYYMMDD/`，AI 画框帧优先），RK3588 硬件编码为 H.264 并封装 MP4(avc1) 文件，分辨率取自摄像头配置；下载界面与日志合并为「文件下载」页（/logs 双 TAB），支持下载、删除、打包下载
- systemd 看门狗：监控关键线程心跳，防止异常卡死
- 热更新配置：Web 页面修改后立即生效
- 运行维护：日志按日期与大小自动轮转（按天分目录、文件名不含重复日期）、清理脚本、部署脚本
- 数据注入 / 提取：REST API 提供 CAN 报文注入（/api/can/send）、DBC 上传热加载、配置写入，以及视频流 / CAN 原始帧与 DBC 解码信号 / 录像 / 日志等数据提取

## 系统架构

单进程多线程架构，所有线程由 `source/core/main.c` 的模块总表 `g_modules[]` 统一注册并拉起：

```text
        ┌─► can_recv_task ──► SocketCAN 收帧 ──► 入 rxq（原始帧）+ 接收方向 DBC 解码
 main ──┼─► can_send_task ──► 排空 txq（原始帧）写 socket + 发送方向 DBC 解码
        ├─► tcp_task      ──► TCP 监听 / 收发
        ├─► http_task     ──► Web 管理 + REST API + MJPEG 推流
        ├─► video_task    ──► V4L2 采集
        ├─► ai_task       ──► 采样帧 → 投递推理队列（多线程池并行推理）→ 画框帧快照
        ├─► rec_task      ──► 网络录像（AI 画框帧优先，回退原始帧）→ H.264 硬件编码 → MP4(avc1)
        └─► watchdog_task ──► 心跳监控 + sd_notify
```

| 线程 | 职责 |
| --- | --- |
| `main` | 初始化、配置加载、DBC 加载、退出编排、喂狗 |
| `can_recv` | CAN 帧接收、入 rxq（原始帧）、接收方向 DBC 解码 |
| `can_send` | 排空 txq（原始帧）写 socket、发送方向 DBC 解码 |
| `tcp` | TCP 监听、连接管理与数据收发 |
| `http` | HTTP 管理服务主循环、REST API、MJPEG 推流 |
| `video` | V4L2 MJPEG 采集与帧发布 |
| `ai` | 帧采样、推理任务投递；模型加载与多线程推理池（每线程独立 rknn context） |
| `rec` | 网络录像：AI 画框帧优先（回退原始帧）→ H.264 硬件编码 → MP4(avc1) 封装落盘 |
| `watchdog` | 线程心跳监控与 `sd_notify` |

> 注：`signal` 模块仅注册信号处理（`init`），`task` 为空，不创建线程。

## 数据流向与接口

### 视频流向

```text
V4L2 摄像头（MJPEG / YUYV）
  └► video_task 采集 ─► 帧快照（video_stream_get_frame，按配置分辨率输出）
        ├► http 线程逐连接推流 ─► /video/mjpeg（原始帧，多部分 MJPEG）
        ├► ai_task 采样 ─► 推理队列 ─► worker×N 并行推理（RKNN）─► 画框帧快照（JPEG）
        │       └► http 线程逐连接推流 ─► /video/mjpeg_ai（AI 不可用回退原始帧）
        └► rec_task 每 20ms 快照 ─► NV12 转换 → H.264 硬件编码（rkvenc）─► MP4(avc1) 封装 ─► recordings/YYYYMMDD/rec_HHMMSS.mp4
```

- 采集：`video_task` 通过 V4L2 打开摄像头，优先 MJPEG 格式，失败回退 YUYV；分辨率取自配置 `video_width` / `video_height`
- 推流：每个 `/video/mjpeg*` 连接一个独立线程，multipart/x-mixed-replace 持续推送 JPEG 帧，慢客户端不阻塞其他连接
- 录像：`rec_task` 默认自动开启，AI 画框帧优先，无 AI 帧回退原始帧（JPEG 解码或 YUYV 直转 NV12），按天分目录落盘；帧率开录前实测（不写死），H.264 码率按 `宽×高×2` 动态计算

视频数据提取接口：

| 接口 | 说明 |
| --- | --- |
| `GET /video/mjpeg` | 原始 MJPEG 视频流（浏览器 `<img src>` 或 ffplay 播放） |
| `GET /video/mjpeg_ai` | AI 画框视频流（检测结果实时叠加） |
| `GET /api/video/caps` | 摄像头能力（分辨率 / 格式） |
| `GET /api/video/devices` | 可用视频设备列表 |

### CAN 数据流向

```text
接收：SocketCAN 帧 ─► can_recv_task（epoll 非阻塞读）
        ├► 原始帧入 rxq（供业务 pop 消费）
        ├► http_can_record_rx ─► /api/can/frames（最近 32 帧）
        └► 接收方向 DBC 解码 ─► /api/can/decoded（最近信号值）

发送：can_tx_frame() 压入 txq ─► eventfd 唤醒 can_send_task ─► 排空写 socket
        ├► 发送方向 DBC 解码 ─► /api/can/decoded/tx
        └► 写失败（EAGAIN）回队尾，队列满记录丢帧
```

- 队列：每接口独立 rxq / txq（原始 `canfd_frame`），DBC 解码与原始帧缓存相互独立
- 故障恢复：读错误自动重建 socket 并重新绑定过滤器；接口断开不阻塞整体启动

CAN 数据注入接口（写数据到总线）：

| 接口 | 说明 |
| --- | --- |
| `POST /api/can/send` | 注入一帧 CAN/CAN FD 报文（body：`ifname=can0&id=0x123&data=01 02 03`，支持 0x 前缀、空格/逗号/连字符分隔） |
| `POST /api/can/toggle` | 控制接口 up / down（body：`ifname=can0&action=up`） |
| `POST /api/can/dbc` | 上传 DBC 数据库（`?ifname=can0`，body 为 DBC 文本，校验通过后热加载并落盘） |
| `POST /api/config` | 写入配置（body 带 `target` 指定模块：`can` / `net` / `video`，只应用该模块参数并重启对应模块；缺省为全量） |

CAN 数据提取接口（从总线读数据）：

| 接口 | 说明 |
| --- | --- |
| `GET /api/can` | CAN 接口状态（up / 波特率 / FD） |
| `GET /api/can/ifaces` | 系统 CAN 接口枚举（含 FD 能力） |
| `GET /api/can/frames` | 最近原始接收帧（id / len / data / 标志） |
| `GET /api/can/decoded` | 接收方向 DBC 解码信号记录 |
| `GET /api/can/decoded/tx` | 发送方向 DBC 解码信号记录 |

### TCP 数据流向

```text
监听：tcp_task 监听配置端口（tcp_port，可选绑定网卡 tcp_bind）
        ├► accept 新连接 ─► 注册 epoll（每客户端独立读写状态）
接收：客户端数据 recv 读取（当前版本读取后丢弃，不注入 CAN 总线）
发送：业务线程压入 txq（tcp_tx_packet） ─► eventfd 唤醒 tcp_task ─► 写客户端 socket
        └► 支持定向发送（client_idx）与广播（client_idx < 0）；慢客户端由每客户端 wbuf 暂存
```

- TCP 服务面向外部客户端：连接数上限 `max_clients`（默认 16），非阻塞 + epoll 管理，部分写由 wbuf 续发
- 发送链路（TX 队列 + eventfd + 广播/定向分发）已就绪，供后续业务注入（如 CAN 帧转发到 TCP 客户端）；当前版本 TCP 接收方向不转发数据

### 其他数据接口

| 接口 | 说明 |
| --- | --- |
| `GET /video/mjpeg_ai` | 见上文视频流向 |
| `GET /api/rec/list` / `GET /recfile/<日期/文件名>` / `GET /api/rec/pack` | 录像数据提取（列表 / 单文件下载 / 打包下载） |
| `GET /api/logs` / `GET /logfile/<日期/文件名>` / `GET /logs/pack` | 日志数据提取 |
| `GET /api/system` | 系统状态（CPU / 内存 / 磁盘 / 温度 / 网络速度） |
| `GET /api/network` / `GET /api/network/ifaces` | 网络状态与接口信息 |
| `POST /api/reboot` / `POST /api/shutdown` | 设备控制（root） |
| `POST /api/rec/start` / `POST /api/rec/stop` | 录像控制（root，默认已自动开启） |

## 目录结构

```text
rk3588-canfd-and-tcp-ip/
├── CMakeLists.txt
├── README.md
├── rk3588-canfd-and-tcp-ip.service  # systemd 服务单元
├── include/                          # 头文件
│   ├── core/
│   │   ├── common.h                  # 应用上下文与公共工具
│   │   ├── config.h                  # 配置结构
│   │   ├── log.h                     # 日志接口
│   │   ├── ring.h                    # 无锁环形队列
│   │   ├── epoll_util.h              # epoll 辅助工具
│   │   └── version.h                 # 版本宏（CMake 生成）
│   ├── can/
│   │   ├── can_queue.h               # CAN 收发队列
│   │   ├── can_socket.h              # SocketCAN 接口
│   │   └── dbc_parser.h              # DBC 解析接口
│   ├── tcp/
│   │   ├── tcp_server.h              # TCP 服务接口
│   │   └── tcp_queue.h               # TCP 发送队列
│   ├── http/
│   │   ├── http.h                    # HTTP 服务接口
│   │   ├── http_internal.h           # HTTP 内部定义
│   │   ├── http_util.h               # HTTP 表单/URL 解析工具
│   │   ├── http_api_can.h            # CAN API 接口声明
│   │   └── http_api_dbc.h            # DBC API 接口声明
│   ├── video/
│   │   ├── video_stream.h            # V4L2 视频流接口
│   │   ├── video_rec.h               # 网络录像模块接口
│   │   ├── h264_encoder.h            # RK3588 硬件 H.264 编码器封装接口（V4L2 M2M rkvenc）
│   │   └── rec_mp4.h                 # MP4(avc1) 封装器接口（纯封装，可独立测试）
│   ├── ai/
│   │   ├── rknn_yolo.h               # RKNN YOLO 框架入口
│   │   ├── yolo_types.h              # 检测结果结构定义
│   │   ├── yolo_image.h              # 图像处理接口（JPEG / YUYV / 缩放）
│   │   ├── yolo_postprocess.h        # 单输出后处理接口
│   │   ├── yolo_draw.h               # 画框接口
│   │   └── rknn_api.h                # RKNN 运行时头文件（rknn-toolkit2 提供）
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
│   │   ├── tcp_server.c              # TCP 服务实现
│   │   └── tcp_queue.c               # TCP 发送队列实现
│   ├── http/
│   │   ├── http.c                    # HTTP 路由与静态文件
│   │   ├── http_util.c               # HTTP 表单/URL 解析工具
│   │   ├── http_api_can.c            # CAN 相关 API
│   │   ├── http_api_dbc.c            # DBC 解码结果 API
│   │   ├── http_api_config.c         # 配置 API
│   │   ├── http_api_network.c        # 网络 API
│   │   ├── http_api_system.c         # 系统 API
│   │   ├── http_api_video.c          # 视频 API
│   │   ├── http_api_rec.c            # 录像 API（start/stop/status/list/delete/pack/下载）
│   │   ├── http_api_ai.c             # AI 文件上传 API（模型 / 类别标签，热重载）
│   │   ├── http_logs.c               # 日志查看
│   │   └── http_reboot.c             # 重启 / 关机
│   ├── video/
│   │   ├── video_stream.c            # V4L2 视频流实现
│   │   ├── video_rec.c               # 网络录像模块实现（录制线程 + 帧获取 + NV12 转换）
│   │   ├── h264_encoder.c            # H.264 硬件编码器实现（V4L2 M2M MPLANE，NV12→H.264）
│   │   └── rec_mp4.c                 # MP4(avc1) 封装器实现
│   ├── ai/
│   │   ├── rknn_yolo.c               # 模型加载 + 多线程推理池 + 乱序重排 + 帧快照
│   │   ├── yolo_image.c              # 图像处理实现（libjpeg / 格式转换 / 缩放）
│   │   ├── yolo_postprocess.c        # YOLO26 单输出后处理（框已解码，阈值 + NMS）
│   │   └── yolo_draw.cpp             # 画框实现（OpenCV cv::rectangle / cv::putText）
│   └── watchdog/
│       └── watchdog.c                # 看门狗实现
├── html/                             # Web 前端
│   ├── index.html                    # 监控页（CAN/TCP 数据网关仪表盘）
│   ├── config.html                   # 配置页
│   ├── dbc.html                      # DBC 解析页
│   ├── logs.html                     # 文件下载页（日志 / 录像双 TAB）
│   ├── css/
│   │   ├── common.css                # 全局共享样式
│   │   ├── config.css                # 配置页样式
│   │   ├── logs.css                  # 文件下载页样式
│   │   └── monitor.css               # 监控页样式
│   └── js/
│       ├── theme.js                  # 主题切换
│       ├── config.js                 # 配置页脚本
│       ├── dbc.js                    # DBC 页脚本
│       ├── logs.js                   # 文件下载页脚本（日志 / 录像）
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
├── recordings/                       # 网络录像输出目录（MP4，运行生成）
└── .gitignore
```

## 依赖要求

- gcc / g++（C11 / C++11，检测框渲染模块为 C++ 并直接调用 OpenCV）
- cmake ≥ 3.10
- libsystemd
- libnl-3
- libnl-route-3
- librknnrt（板载 RKNN 运行时，模型由 rknn-toolkit2 在 PC 上转换）
- libjpeg（JPEG 解码与 AI 画框用）
- OpenCV 4（core + imgproc，检测框画框与标签文字渲染）
- RK3588 硬件编码器 `/dev/video-enc0`（rkvenc，Linux V4L2 M2M 驱动，录像 H.264 编码用）
- Linux SocketCAN 支持

安装示例：

```bash
apt install build-essential cmake libsystemd-dev libnl-3-dev libnl-route-3-dev libjpeg62-turbo-dev libopencv-dev
```

## 构建

```bash
cd /home/orangepi/rk3588-canfd-and-tcp-ip
./scripts/build.sh -R   # Release（默认）
./scripts/build.sh -D   # Debug
./scripts/build.sh -C   # 清理构建产物与 logs
```

`build.sh` 构建前会自动检查依赖（工具链 + 开发库 + librknnrt），缺失时打印具体缺项与安装命令并中止，避免在 cmake / make 阶段才暴露晦涩报错；`libavcodec-dev`（FFmpeg rkmpp 后端）为可选依赖，缺失时自动回退 V4L2 rkvenc 编码。

构建产物：`./bin/rk3588-canfd-and-tcp-ip`

版本信息由 CMake 在配置阶段生成到 `include/core/version.h`，包括 `APP_NAME`、`APP_VERSION`、`APP_GIT_COMMIT`、`APP_GIT_BRANCH`、`APP_GIT_DIRTY`、`APP_BUILD_TYPE`、`APP_BUILD_DATE`。

## 运行方式

```bash
cd /home/orangepi/rk3588-canfd-and-tcp-ip
./bin/rk3588-canfd-and-tcp-ip
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

`deploy.sh` 需 root 权限，安装目录为 `/opt/rk3588-canfd-and-tcp-ip/`，部署内容如下：

| 源 | 目标 |
| --- | --- |
| `bin/<可执行文件>` | `/opt/.../bin/` |
| `html/` | `/opt/.../html/` |
| systemd 服务单元 | `/etc/systemd/system/` |

> 注：`config/config.txt` 与 `config/` 下的 DBC 模板文件**不会**被复制；首次部署使用程序内置默认值，后续通过 Web 配置页保存配置、上传 DBC 文件。`logs/` 与 `recordings/` 目录由部署脚本创建。

服务名称：`rk3588-canfd-and-tcp-ip`

查看服务状态：

```bash
systemctl status rk3588-canfd-and-tcp-ip
journalctl -u rk3588-canfd-and-tcp-ip -f
```

部署要点：

- `Type=notify`
- `WatchdogSec=10`
- `Restart=on-failure`（失败自动重启，间隔 5s）
- `AmbientCapabilities=CAP_NET_ADMIN CAP_NET_RAW`
- `ProtectSystem=strict`（文件系统只读，仅 `config/`、`logs/`、`recordings/` 可写，`html/` 只读）

## 配置说明

`config/config.txt` 由 Web 页面保存，并在运行时生效。

| 键 | 格式 | 说明 |
| --- | --- | --- |
| `can_ifname` | `<name>` | CAN 接口名称，可重复；未提供时自动从系统枚举实际存在的 CAN 接口 |
| `can_bitrate` | `<name> <bps>` | 比特率，默认 500000 |
| `can_dbitrate` | `<name> <bps>` | FD 数据比特率，默认 2000000 |
| `can_fd` | `<name> on\|off` | CAN FD 开关 |
| `can_up` | `<name> on\|off` | 启动时 bring up |
| `can_filter` | `<name> <id> <mask>` | 按 CAN 通道设置接收过滤器（16 进制 id / mask，可重复添加） |
| `tcp_port` | `<port>` | TCP 端口，默认 6666 |
| `max_clients` | `<n>` | 最大客户端数，默认 16 |
| `tcp_bind` | `<ifname>` | TCP 监听绑定网卡名，留空表示绑定所有网卡（`INADDR_ANY`） |
| `video_device` | `<path>` | 视频设备，默认 `/dev/video0` |
| `video_width` / `video_height` | `<n>` | 分辨率，默认 640×480 |
| `video_fps` | `<n>` | 期望帧率（1~120，V4L2 S_PARM 设置驱动帧间隔）；0 = 驱动默认 |
| `can_dbc` | `<name> <path>` | 按 CAN 通道配置 DBC 数据库文件路径，留空则不启用该通道信号解码 |
| `ai_enable` | `on\|off` | 启用 NPU 推理画框流，默认 `off` |
| `ai_model` | `<path>` | YOLO26 官方单输出 `.rknn` 模型路径，默认 `config/yolo26.rknn` |
| `ai_names` | `<path>` | 类别标签文件路径（COCO 格式，每行一个类名），默认 `config/coco.names` |
| `ai_input_size` | `<n>` | 模型输入边长（32~2048），默认 640 |
| `ai_conf` | `<n>` | 置信度阈值百分比（1~100），默认 25（0.25） |
| `ai_nms` | `<n>` | NMS IoU 阈值百分比（1~100），默认 45（0.45） |
| `ai_interval_ms` | `<n>` | 推理节流间隔（10~5000ms），默认 10（每帧推理） |
| `ai_threads` | `<n>` | 推理工作线程数（3 的倍数：3~15，自动向下取整），默认 3；每线程独立 rknn context，worker i 绑定 NPU 核 i%3 |

未提供配置文件时，将从系统枚举实际存在的 CAN 接口（netlink 路由，`kind=="can"`）并套用默认参数；若系统无法枚举到接口，则回退到 `can0` / `can1`。

### DBC 模板

`config/` 下提供两个 CAN 通道的 DBC 模板文件，可通过 `can_dbc <通道名> <路径>` 启用（或在 Web 配置页上传）：

- `config/can0.dbc`：动力 / 电池类（`EngineData`、`BatteryStatus`）
- `config/can1.dbc`：底盘 / 位置类（`Status` 含 Motorola 大端信号、`VehiclePos` 扩展帧）

> 解析器只识别 `BO_`（报文）与 `SG_`（信号）两类行，其余（含 `//` 注释）均忽略。ID 为十进制；扩展帧需在 29 位 ID 上置 bit31（如 `0x18FF50E5` → `2566869221`）。

## Web 管理界面

服务默认监听 HTTP 端口 80，提供以下入口：

- `/`：仪表盘（视频流下方实时显示客户端实测帧率徽标）
- `/dbc`：DBC 信号解析
- `/config`：运行配置
  - 视频卡：设备 / 格式分辨率下拉框按相机真实能力枚举（`/api/video/caps`），并显示该格式与分辨率下驱动支持的 FPS 档位，可选择保存（`video_fps`）后立即生效
  - AI 卡：推理参数与模型文件上传左右分栏（1:1），上传热生效，并显示当前加载的模型 / 标签文件名
  - CAN 卡：波特率 / DBC 上传 / CAN FD / FD 波特率一行配置，通道下拉框标注 FD 能力
  - 网络卡：IP 模式（关闭 / 静态 / DHCP），仅「静态」模式显示 IP / 掩码 / 网关输入框
  - 顶部「导出配置表」：一键导出全部配置为可读文本（含原始 JSON），便于备份与移植
- `/logs`：文件下载（日志 / 录像双 TAB）

### REST API

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | `/api/system` | 无 | 系统监控数据 |
| GET | `/api/can` | 无 | CAN 接口状态 |
| GET | `/api/can/ifaces` | 无 | 系统 CAN 接口枚举（含 FD 能力） |
| GET | `/api/can/decoded` | 无 | 接收方向 DBC 解析后的最近 CAN 信号（JSON） |
| GET | `/api/can/decoded/tx` | 无 | 发送方向 DBC 解析后的最近 CAN 信号（JSON） |
| GET | `/api/can/frames` | 无 | 最近接收的原始 CAN 帧 |
| POST | `/api/can/send` | root | 发送 CAN 帧 |
| POST | `/api/can/dbc?ifname=` | root | 上传某通道的 DBC 文件 |
| POST | `/api/can/toggle` | root | 切换 CAN 接口开关 |
| POST | `/api/ai/upload?type=model` | root | 上传 YOLO26 `.rknn` 模型（≤64MB，校验 RKNN magic），原子替换后热重载推理池 |
| POST | `/api/ai/upload?type=names` | root | 上传类别标签文件（≤256KB，1~128 行，每行 ≤23 字符），原子替换后热重载 |
| GET/POST | `/api/config` | root | 读取 / 写入配置 |
| GET | `/api/network` | 无 | 网络统计 |
| GET | `/api/network/ifaces` | 无 | 网络接口信息 |
| GET | `/api/video/devices` | 无 | 摄像头设备列表 |
| GET | `/api/video/caps` | 无 | 视频参数列表 |
| GET | `/video/mjpeg` | 无 | MJPEG 视频流 |
| GET | `/video/mjpeg_ai` | 无 | AI 画框 MJPEG 视频流（AI 未启用时回退原始帧） |
| POST | `/api/rec/start` | root | 手动开始网络录像（默认已自动开启，一般无需调用） |
| POST | `/api/rec/stop` | root | 手动停止网络录像（自动写 moov 收尾；停止后不再自动续录） |
| GET | `/api/rec/status` | 无 | 录制状态（录制中 / 文件名 / 帧数 / 字节数 / 帧率） |
| GET | `/api/rec/list` | 无 | 录像文件列表（按时间倒序；`日期/文件名` 两段式，按天分目录） |
| POST | `/api/rec/delete` | root | 删除录像文件（body 为 `日期/文件名`） |
| GET | `/recfile/<日期/文件名>` | root | 下载录像文件 |
| GET | `/api/rec/pack` | root | 打包下载全部录像（tar.gz，上限 2GB） |
| GET | `/api/logs`、`/logs`、`/logfile/*` | root | 日志列表 / 下载 / 删除 |
| GET | `/logs/pack` | root | 打包下载全部日志（tar.gz） |

> 认证说明：监控页（`/`）与 DBC 页（`/dbc`）需要普通用户认证；配置页（`/config`）、文件下载页（`/logs`）与所有写操作接口要求管理员权限（root 或 sudo 组成员，如 orangepi）。

## AI 目标检测

RKNN + YOLO26（官方 ultralytics 单输出格式 `(1, 84, 8400)`，fp16，官方 mean/std），由 `rknn-toolkit2` 转换后在板载 NPU 推理：

- 模型加载：`ai_model` 指向 `.rknn` 文件，校验输出必须为单个检测头 `(1, 84, 8400)`；类别名由 `ai_names` 加载（COCO 格式，每行一个类名，缺失回退内置 COCO 80 类）
- 流水线：`ai_task` 按 `ai_interval_ms` 采样采集帧（seq 去重）→ 有界任务队列（容量 4）→ N 个推理 worker（独立 rknn context，worker i 绑定 NPU 核 i%3，RK3588 三核等量分配）→ 单 composer 线程严格按 seq 顺序消费结果（乱序重排、100ms 超时跳缺口、seq 回绕回退游标）→ EMA 平滑（α=0.4，同类 IoU≥0.3 匹配）→ 渲染 → 快照
- 画框渲染：直接调用 OpenCV（`cv::rectangle` 3px 实色 + LINE_AA 抗锯齿；`cv::putText` Hershey Simplex 白色粗体字 + 实色底块，默认挂在框顶外侧，顶部不足落框内），每一帧都输出标注帧，不混入未推理的原始帧
- 热更新：`ai_enable` / `ai_threads` / `ai_conf` / `ai_nms` / `ai_interval_ms` 保存后立即重建推理池生效，无需重启进程；`ai_model` / `ai_names` 可通过 Web 配置页或 API 上传（校验 RKNN magic / 标签格式后原子替换文件并热重载）
- 优雅降级：模型缺失 / NPU 驱动未加载 / 推理失败时 `enabled=0`，原视频流照常，画框流回退原始帧，不崩溃不阻断启动

### RKNN 转换工作流

在 PC（x86_64，Python 3.10）上用 rknn-toolkit2 将官方 YOLO26 权重转为板端模型：

1. 获取权重与工具：从 ultralytics 官方发布页下载 YOLO26 权重（如 `yolo26n.pt`）；安装 `rknn-toolkit2`（含 `rknn-toolkit-lite2` 用于板端验证）与 `ultralytics`
2. 导出 ONNX：用 ultralytics 官方脚本导出检测头（640×640，单输出，输出张量 `(1, 84, 8400)`，84 = 4 框 + 80 类）
3. 转换 RKNN：`rknn.config(mean_values=官方默认, std_values=官方默认, target_platform='rk3588')`，`hybrid_quantization_step2` 不启用，导出 fp16 权重（推荐）或 int8（需量化数据集）；转换后校验输出 shape 必须为 `(1, 84, 8400)`
4. 板端验证：在 RK3588 上用 `rknn-toolkit-lite2` 加载推理，与 ONNX/PT 输出对拍（允许 fp16 微小误差）；`rknn_init` 通过后即可部署
5. 部署：Web 配置页上传 `yolo26.rknn`（或放入 `config/` 并设置 `ai_model`），同时上传类别标签文件（或 `config/coco.names`）；日志输出 `ai: model '...' loaded, classes '...' (80), input 640x640, single output, threads N (NPU core i%3)` 即转换正确

> 仓库 `config/yolo26.rknn` 为 yolo26n 官方权重转换（fp16，单输出 (1,84,8400)，官方 mean/std，无内置 NMS）。

## 网络录像

服务启动后默认自动录制，依赖 RK3588 硬件 H.264 编码器（`/dev/video-enc0`，rkvenc）：

- 触发方式：默认自动开启（无需 Web 操作）；也保留 `/api/rec/start` / `/api/rec/stop` 手动接口（手动停止后不再自动续录）
- 按天分目录：录制文件存于 `recordings/YYYYMMDD/`，每天一个子目录，自动跨天切换；文件名规范化只保留时间（`rec_HHMMSS.mp4`，同秒冲突自动追加序号），日志文件同理为 `rk3588-canfd-and-tcp-ip_info.log` / `_error.log`
- 分辨率：录制分辨率取摄像头配置 `video_width` / `video_height` 的选定值（任意分辨率），未配置时退回首帧探测
- 帧率：开录前连续抓帧实测（不写死），钳制在 1~120 fps
- 编码链路：帧 → 转 NV12（AI 画框帧 JPEG 解码 / 原始 YUYV 直转）→ `/dev/video-enc0` 硬件编码 H.264（CBR，码率 `宽×高×2`，钳制 300k~16M，GOP = fps×2）→ 封装
- 封装格式：标准 ISO BMFF MP4（avc1 track，SPS/PPS 取自编码器，stss 记录 IDR 关键帧），`ftyp + mdat + moov`，moov 末尾回写记录每帧偏移与时长，Chrome / VLC / ffplay 均可播放
- 下载管理：与日志下载界面合并为「文件下载」页（`/logs`），TAB 切换日志 / 录像列表，支持下载、删除、打包下载（日志 `/logs/pack`、录像 `/api/rec/pack`，录像打包上限 2GB；文件名白名单 + 两段式路径校验防穿越）
- 上限保护：单次录制帧数与 mdat 体积上限（防止 32 位 size 溢出），达到上限自动收尾并续录下一段

## 看门狗机制

- 通过模块总表 `g_modules[]` 为 `can_recv`、`can_send`、`tcp`、`http`、`video`、`ai`、`rec`、`main` 注册心跳监控
- 每个模块带 `timeout` / `max_miss` 参数；`watchdog` 线程自身 `timeout=0`，不监督自己
- 线程以名字注册 / 喂狗 / 注销，超时日志可直接定位到具体线程名（如 `thread 'http'`）
- 心跳超时将触发整体退出
- 每 5 秒调用 `sd_notify("WATCHDOG=1")`（`WatchdogSec=10` 的一半）

## 日志管理

日志按级别与日期归档，按天分目录（日期已体现在目录名，文件名不再重复日期），单文件超过 10MB 自动轮转。

```text
logs/
└── YYYYMMDD/
    ├── rk3588-canfd-and-tcp-ip_info.log
    └── rk3588-canfd-and-tcp-ip_error.log
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
