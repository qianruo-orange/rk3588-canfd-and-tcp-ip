#ifndef CONFIG_H
#define CONFIG_H

#include <linux/can.h>

#include "core/common.h"  /* PATH_CONFIG */

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
} can_iface_t;

/* ---- 网关启动参数 ---- */
typedef struct {
    can_iface_t can_ifaces[CAN_MAX_IFACES];
    int         can_count;
    int         tcp_port;
    int         max_clients;
} gateway_args_t;

struct app_config_t {
    gateway_args_t gw_args;
    int            wd_sec;
    char           log_dir[256];
    char           video_device[128];
    int            video_width;
    int            video_height;
    int            http_port;   /* Web 管理端口（默认 80） */
};

int  config_load(struct app_config_t *cfg);
void config_defaults(struct app_config_t *cfg);
void config_save(app_ctx_t *app);

#endif /* CONFIG_H */
