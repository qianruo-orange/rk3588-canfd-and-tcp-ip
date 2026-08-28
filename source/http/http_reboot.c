/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <sys/reboot.h>
#include <unistd.h>
/**
 * http_reboot.c — 系统重启 / 关机 API。
 */

#include "http/http_internal.h"

/* 重启/关机共用同一套响应与执行流程，仅页面文案与动作不同 */
static void reboot_action(int fd, int power_off)
{
    char html[512];
    int n = snprintf(html, sizeof(html),
        "<html><body style='font-family:Segoe UI;background:#0d1117;"
        "color:#c9d1d9;padding:40px;text-align:center'>"
        "<h2 style='color:%s'> %s...</h2><p>%s</p></body></html>",
        power_off ? "#da3633" : "#58a6ff",
        power_off ? "正在关机" : "正在重启",
        power_off ? "设备即将关闭" : "设备将在几秒后重启");
    http_send_response(fd, 200, "OK", "text/html; charset=utf-8", html, (size_t)n);
    sync();
    reboot(power_off ? RB_POWER_OFF : RB_AUTOBOOT);
}

void http_reboot(app_ctx_t *app, int fd)
{
    (void)app;
    reboot_action(fd, 0);
}

void http_shutdown(app_ctx_t *app, int fd)
{
    (void)app;
    reboot_action(fd, 1);
}
