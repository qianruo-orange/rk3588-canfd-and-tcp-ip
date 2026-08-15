/**
 * config.c — 配置持久化（文件 ↔ 运行时双向同步）
 * 启动时从 config.txt 加载，若无则用默认值；
 * 保存时同时写入文件和读取运行时上下文（app_ctx_t）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "core/config.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"
#include "core/log.h"
#include "core/version.h"

#define CONFIG_PATH PATH_CONFIG

void config_defaults(struct app_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    safe_strncpy(cfg->log_dir, sizeof(cfg->log_dir), PATH_LOGS);
    safe_strncpy(cfg->video_device, sizeof(cfg->video_device), "/dev/video0");
    cfg->video_width  = 640;
    cfg->video_height = 480;
    cfg->http_port    = 80;   /* 与 http.h 的 HTTP_DEFAULT_PORT 一致 */

    gateway_args_t *args = &cfg->gw_args;
    args->tcp_port    = 6666;
    args->max_clients = 16;

    can_iface_t *c0 = &args->can_ifaces[args->can_count++];
    memset(c0, 0, sizeof(*c0)); c0->sock_fd = -1;
    snprintf(c0->ifname, sizeof(c0->ifname), "%s", "can0");
    c0->bitrate = 500000; c0->dbitrate = 2000000; c0->fd_mode = 1; c0->up = 1;

    can_iface_t *c1 = &args->can_ifaces[args->can_count++];
    memset(c1, 0, sizeof(*c1)); c1->sock_fd = -1;
    snprintf(c1->ifname, sizeof(c1->ifname), "%s", "can1");
    c1->bitrate = 500000; c1->dbitrate = 2000000; c1->fd_mode = 1; c1->up = 1;

    log_info("config: defaults loaded");
}

int config_load(struct app_config_t *cfg)
{
    FILE *fp = fopen(CONFIG_PATH, "r");
    if (!fp) { config_defaults(cfg); return 0; }

    memset(cfg, 0, sizeof(*cfg));
    safe_strncpy(cfg->log_dir, sizeof(cfg->log_dir), PATH_LOGS);
    cfg->video_width = 640; cfg->video_height = 480;
    cfg->http_port   = 80;   /* 与 http.h 的 HTTP_DEFAULT_PORT 一致 */
    gateway_args_t *a = &cfg->gw_args;

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
        else if (strcmp(key, "video_device") == 0) safe_strncpy(cfg->video_device, sizeof(cfg->video_device), val);
        else if (strcmp(key, "video_width") == 0) cfg->video_width = parse_int_clamped(val, 1, 4096, 640);
        else if (strcmp(key, "video_height") == 0) cfg->video_height = parse_int_clamped(val, 1, 4096, 480);
        else if (strcmp(key, "dbc_path") == 0) safe_strncpy(cfg->dbc_path, sizeof(cfg->dbc_path), val);
        else if (strcmp(key, "http_port") == 0) cfg->http_port = parse_int_clamped(val, 1, 65535, 80);
    }
    fclose(fp);
    if (a->can_count == 0) { config_defaults(cfg); return 0; }
    if (a->tcp_port <= 0) a->tcp_port = 6666;
    log_info("config: loaded from %s", CONFIG_PATH);
    return 0;
}

void config_save(app_ctx_t *app)
{
    if (!app || !app->cfg || !app->can || !app->tcp) return;
    struct app_config_t *cfg = app->cfg;
    can_ctx_t *can = app->can;
    tcp_ctx_t *tcp = app->tcp;

    FILE *fp = fopen(CONFIG_PATH, "w");
    if (!fp) { log_error("config: cannot write %s", CONFIG_PATH); return; }

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
    fprintf(fp, "\n# --- 视频 ---\n");
    fprintf(fp, "video_device %s\n", cfg->video_device);
    fprintf(fp, "video_width %d\n", cfg->video_width);
    fprintf(fp, "video_height %d\n", cfg->video_height);
    fprintf(fp, "\n# --- DBC ---\n");
    if (cfg->dbc_path[0]) fprintf(fp, "dbc_path %s\n", cfg->dbc_path);
    fprintf(fp, "\n# --- HTTP ---\n");
    fprintf(fp, "http_port %d\n", cfg->http_port);

    fclose(fp);
    log_info("config: saved to %s", CONFIG_PATH);
}
