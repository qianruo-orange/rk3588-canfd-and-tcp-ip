/**
 * config.c — 配置持久化（文件 ↔ 运行时双向同步）
 * 启动时从 config.txt 加载，若无则用默认值；
 * 保存时同时写入文件和读取运行时上下文（app_ctx_t）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "core/config.h"
#include "can/can_socket.h"
#include "tcp/tcp_server.h"
#include "core/log.h"
#include "core/version.h"

#define CONFIG_PATH PATH_CONFIG

/* 串行化配置持久化：http_config_post 与 http_can_dbc_upload 等
   并发保存时避免写坏 config.txt */
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;

static void config_defaults(struct app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    safe_strncpy(cfg->log_dir, sizeof(cfg->log_dir), PATH_LOGS);
    safe_strncpy(cfg->video_device, sizeof(cfg->video_device), "/dev/video0");
    cfg->video_width  = 640;
    cfg->video_height = 480;

    app_args_t *args = &cfg->args;
    args->tcp_port    = 6666;
    args->max_clients = 16;

    /* 可配置 CAN 通道：优先从系统读取实际存在的 CAN 接口 */
    char names[CAN_MAX_IFACES][IFNAMSIZ];
    int n = can_enumerate_system(names, CAN_MAX_IFACES);
    if (n <= 0) {
        /* 无法枚举系统接口时回退到 can0/can1，保证默认可用 */
        snprintf(names[0], sizeof(names[0]), "%s", "can0");
        snprintf(names[1], sizeof(names[1]), "%s", "can1");
        n = 2;
    }

    for (int i = 0; i < n && args->can_count < CAN_MAX_IFACES; i++) {
        can_iface_t *c = &args->can_ifaces[args->can_count++];
        memset(c, 0, sizeof(*c)); c->sock_fd = -1;
        snprintf(c->ifname, sizeof(c->ifname), "%s", names[i]);
        c->bitrate = 500000; c->dbitrate = 2000000; c->fd_mode = 1; c->up = 1;
    }

    LOG_INFO("config: defaults loaded (%d can iface(s))", args->can_count);
}

int config_load(struct app_config_t *cfg)
{
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (!fp) { config_defaults(cfg); return 0; }

    memset(cfg, 0, sizeof(*cfg));
    safe_strncpy(cfg->log_dir, sizeof(cfg->log_dir), PATH_LOGS);
    cfg->video_width = 640; cfg->video_height = 480;
    app_args_t *a = &cfg->args;

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *c = strchr(line, '#'); if (c) *c = '\0';
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' || line[len-1] == ' ')) line[--len] = '\0';
        if (len == 0) continue;

        char key[64], val[256];
        if (sscanf(line, "%63s %255[^\n]", key, val) != 2) continue;

        if (strcmp(key, "can_ifname") == 0 && a->can_count < CAN_MAX_IFACES) {
            can_iface_t *ifc = &a->can_ifaces[a->can_count++];
            memset(ifc, 0, sizeof(*ifc)); ifc->sock_fd = -1;
            safe_strncpy(ifc->ifname, sizeof(ifc->ifname), val);
        } else if (strcmp(key, "can_bitrate") == 0) {
            char nm[64]; int r; if (sscanf(val, "%63s %d", nm, &r) == 2)
                for (int i = 0; i < a->can_count; i++) if (!strcmp(a->can_ifaces[i].ifname, nm)) { a->can_ifaces[i].bitrate = r; break; }
        } else if (strcmp(key, "can_dbitrate") == 0) {
            char nm[64]; int r; if (sscanf(val, "%63s %d", nm, &r) == 2)
                for (int i = 0; i < a->can_count; i++) if (!strcmp(a->can_ifaces[i].ifname, nm)) { a->can_ifaces[i].dbitrate = r; break; }
        } else if (strcmp(key, "can_fd") == 0) {
            char nm[64], m[8]; if (sscanf(val, "%63s %7s", nm, m) == 2)
                for (int i = 0; i < a->can_count; i++) if (!strcmp(a->can_ifaces[i].ifname, nm)) { a->can_ifaces[i].fd_mode = !strcmp(m, "on"); break; }
        } else if (strcmp(key, "can_up") == 0) {
            char nm[64], m[8]; if (sscanf(val, "%63s %7s", nm, m) == 2)
                for (int i = 0; i < a->can_count; i++) if (!strcmp(a->can_ifaces[i].ifname, nm)) { a->can_ifaces[i].up = !strcmp(m, "on"); break; }
        } else if (strcmp(key, "tcp_port") == 0) a->tcp_port = parse_int_clamped(val, 1, 65535, 6666);
        else if (strcmp(key, "max_clients") == 0) a->max_clients = parse_int_clamped(val, 1, TCP_MAX_CLIENTS, 16);
        else if (strcmp(key, "tcp_bind") == 0) safe_strncpy(a->tcp_bind, sizeof(a->tcp_bind), val);
        else if (strcmp(key, "video_device") == 0) safe_strncpy(cfg->video_device, sizeof(cfg->video_device), val);
        else if (strcmp(key, "video_width") == 0) cfg->video_width = parse_int_clamped(val, 1, 4096, 640);
        else if (strcmp(key, "video_height") == 0) cfg->video_height = parse_int_clamped(val, 1, 4096, 480);
        else if (strcmp(key, "can_dbc") == 0) {
            char nm[64], path[256];
            if (sscanf(val, "%63s %255s", nm, path) == 2)
                for (int i = 0; i < a->can_count; i++) if (!strcmp(a->can_ifaces[i].ifname, nm)) { safe_strncpy(a->can_ifaces[i].dbc_path, sizeof(a->can_ifaces[i].dbc_path), path); break; }
        }
    }
    fclose(fp);
    if (a->can_count == 0) { config_defaults(cfg); return 0; }
    if (a->tcp_port <= 0) a->tcp_port = 6666;
    LOG_INFO("config: loaded from %s", CONFIG_PATH);
    return 0;
}

void config_save(app_ctx_t *app)
{
    if (!app || !app->cfg || !app->can || !app->tcp) return;
    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    pthread_mutex_lock(&g_config_mutex);
    /* 原子写：先写临时文件，再 rename 覆盖，避免进程崩溃留下半截配置 */
    char tmp_path[sizeof(CONFIG_PATH) + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", CONFIG_PATH);
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        LOG_ERROR("config: cannot write %s", tmp_path);
        pthread_mutex_unlock(&g_config_mutex);
        return;
    }

    fprintf(fp, "# %s — auto-saved\n", APP_NAME);
    for (int i = 0; i < can->count; i++)
        fprintf(fp, "can_ifname %s\n", can->ifaces[i].ifname);

    for (int i = 0; i < can->count; i++) {
        can_iface_t *iface = &can->ifaces[i];
        fprintf(fp, "\n# --- %s ---\n", iface->ifname);
        fprintf(fp, "can_bitrate %s %d\n", iface->ifname, iface->bitrate);
        fprintf(fp, "can_fd %s %s\n", iface->ifname, iface->fd_mode ? "on" : "off");
        fprintf(fp, "can_dbitrate %s %d\n", iface->ifname, iface->dbitrate);
        fprintf(fp, "can_up %s %s\n", iface->ifname, iface->up ? "on" : "off");
    }

    fprintf(fp, "\ntcp_port %d\nmax_clients %d\n", tcp->port, tcp->max_clients);
    if (tcp->bind_ifname[0])
        fprintf(fp, "tcp_bind %s\n", tcp->bind_ifname);
    fprintf(fp, "\n# --- 视频 ---\n");
    fprintf(fp, "video_device %s\n", cfg->video_device);
    fprintf(fp, "video_width %d\n", cfg->video_width);
    fprintf(fp, "video_height %d\n", cfg->video_height);
    fprintf(fp, "\n# --- DBC ---\n");
    for (int i = 0; i < can->count; i++)
        if (can->ifaces[i].dbc_path[0])
            fprintf(fp, "can_dbc %s %s\n", can->ifaces[i].ifname, can->ifaces[i].dbc_path);

    if (fclose(fp) != 0 || rename(tmp_path, CONFIG_PATH) != 0) {
        LOG_ERROR("config: save failed (%s)", CONFIG_PATH);
        remove(tmp_path);
    } else {
        LOG_INFO("config: saved to %s", CONFIG_PATH);
    }
    pthread_mutex_unlock(&g_config_mutex);
}
