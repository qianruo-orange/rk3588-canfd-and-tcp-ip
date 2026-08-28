/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef CONFIG_H
#define CONFIG_H

#include <linux/can.h>
#include <linux/if.h>

#include "core/common.h"  /* PATH_CONFIG */
#include "can/can_queue.h"

#define DEFAULT_CONFIG_PATH PATH_CONFIG

#define CAN_MAX_IFACES   8
#define CAN_MAX_FILTERS  16

/* ---- CAN 过滤器 ---- */
typedef struct {
    canid_t id;
    canid_t mask;
} can_filter_t;

/* ---- CAN 接口配置 ---- */
typedef struct {
    char          ifname[16];
    int           sock_fd;
    int           filter_count;
    can_filter_t  filters[CAN_MAX_FILTERS];
    int           bitrate;
    int           dbitrate;
    int           fd_mode;
    int           up;
    int           restart_ms;
    char          dbc_path[256]; /* 该通道的 DBC 文件路径（空 = 不启用解码） */
    can_queue_t   txq; /* 该接口发送队列 */
    can_queue_t   rxq; /* 该接口接收队列 */
} can_iface_t;

/* 按接口名查找下标（-1 = 不存在）；config.c 解析 6 处、http_api_can.c 2 处、
   can_socket.c 1 处共用的同一段线性查找收口 */
static inline int can_iface_index(const can_iface_t *ifaces, int count, const char *name)
{
    if (!ifaces || !name) return -1;
    for (int i = 0; i < count; i++)
        if (strcmp(ifaces[i].ifname, name) == 0) return i;
    return -1;
}

/* ---- 应用启动参数 ---- */
typedef struct {
    can_iface_t can_ifaces[CAN_MAX_IFACES];
    int         can_count;
    int         tcp_port;
    int         max_clients;
    char        tcp_bind[IFNAMSIZ]; /* TCP 监听绑定网卡名（空 = 绑定所有网卡 INADDR_ANY） */

    /* ---- 网卡 IP 设置（配置页 IP 卡片，static/dhcp） ---- */
    char        ip_ifname[IFNAMSIZ]; /* 管理的网卡（空 = 不管理） */
    char        ip_mode[8];          /* "static" / "dhcp" / "" */
    char        ip_addr[32];         /* 静态 IPv4 地址 */
    char        ip_mask[32];         /* 子网掩码（点分） */
    char        ip_gw[32];           /* 网关 */
} app_args_t;

struct app_config_t {
    app_args_t    args;
    int           wd_sec;
    char          log_dir[256];
    char          video_device[128];
    int           video_width;
    int           video_height;
    int           video_fps;       /* 期望帧率；0 = 驱动默认 */

    /* ---- RKNN AI 检测（YOLO26，必要流程）---- */
    char          ai_model[256];    /* .rknn 模型文件路径 */
    char          ai_names[256];    /* 类别名文件（coco.names 格式，每行一个类名） */
    int           ai_input_size;    /* 模型输入边长（动态模型用；静态模型以模型实际尺寸为准） */
    float         ai_conf;          /* 置信度阈值 */
    float         ai_nms;           /* NMS IoU 阈值 */
    int           ai_interval_ms;   /* 推理节流间隔（ms），默认 10（每帧推理） */
    int           ai_threads;       /* 推理工作线程数（3 的倍数：3~15，默认 3），每线程独立 rknn context，按 i%3 绑定 NPU 核 */
};

int  config_load(struct app_config_t *cfg);
void config_save(app_ctx_t *app);

#endif /* CONFIG_H */
