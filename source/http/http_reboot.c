#include <sys/reboot.h>
#include <unistd.h>
/**
 * http_reboot.c — 系统重启 / 关机 API。
 */

#include "http/http_internal.h"

void http_reboot(app_ctx_t *app, int fd)
{
    (void)app;
    const char *msg = "<html><body style='font-family:Segoe UI;background:#0d1117;"
        "color:#c9d1d9;padding:40px;text-align:center'>"
        "<h2 style='color:#58a6ff'> 正在重启...</h2><p>设备将在几秒后重启</p>"
        "</body></html>";
    http_send_response(fd, 200, "OK", "text/html; charset=utf-8", msg, strlen(msg));
    sync();
    reboot(RB_AUTOBOOT);
}

void http_shutdown(app_ctx_t *app, int fd)
{
    (void)app;
    const char *msg = "<html><body style='font-family:Segoe UI;background:#0d1117;"
        "color:#c9d1d9;padding:40px;text-align:center'>"
        "<h2 style='color:#da3633'> 正在关机...</h2><p>设备即将关闭</p>"
        "</body></html>";
    http_send_response(fd, 200, "OK", "text/html; charset=utf-8", msg, strlen(msg));
    sync();
    reboot(RB_POWER_OFF);
}
