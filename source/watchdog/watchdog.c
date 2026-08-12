#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <systemd/sd-daemon.h>
#include "watchdog/watchdog.h"
#include "core/common.h"
#include "core/log.h"

typedef struct {
    const char *name;
    int    beat, timeout, max_miss, miss_count, last_beat;
    time_t last_advance;
} wd_entry_t;

static wd_entry_t g_wd_slots[WD_MAX_SLOTS] = {
    [WD_CAN]   ={.name="can",   .timeout=3,  .max_miss=3},
    [WD_TCP]   ={.name="tcp",   .timeout=5,  .max_miss=3},
    [WD_HTTP]  ={.name="http",  .timeout=5,  .max_miss=3},
    [WD_VIDEO] ={.name="video", .timeout=5,  .max_miss=3},
    [WD_MAIN]  ={.name="main",  .timeout=15, .max_miss=1},
};
static int wd_notify_sec;

int watchdog_init(void *arg)
{
    (void)arg;
    const char *env_usec = getenv("WATCHDOG_USEC");
    if (env_usec) { long usec = atol(env_usec); if (usec > 0) { wd_notify_sec = (int)(usec / 2000000); if (wd_notify_sec < 1) wd_notify_sec = 1; log_info("watchdog: systemd WATCHDOG_USEC=%ld notify=%ds", usec, wd_notify_sec); } }
    return 0;
}

void *watchdog_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    time_t now = time(NULL);
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (!g_wd_slots[i].name) continue;
        g_wd_slots[i].last_beat = __atomic_load_n(&g_wd_slots[i].beat, __ATOMIC_RELAXED);
        g_wd_slots[i].last_advance = now;
    }
    log_info("watchdog started");
    sd_notify(0, "READY=1");
    time_t next_notify = 0; int notify_fail = 0;
    if (wd_notify_sec > 0) { sd_notify(0, "WATCHDOG=1"); next_notify = time(NULL) + wd_notify_sec; }
    while (app->running) {
        sleep(1); now = time(NULL);
        for (int i = 0; i < WD_MAX_SLOTS; i++) {
            if (!g_wd_slots[i].name) continue;
            int timeout = g_wd_slots[i].timeout; if (timeout <= 0) continue;
            int cur = __atomic_load_n(&g_wd_slots[i].beat, __ATOMIC_RELAXED);
            if (cur != g_wd_slots[i].last_beat) { g_wd_slots[i].last_beat = cur; g_wd_slots[i].last_advance = now; g_wd_slots[i].miss_count = 0; }
            else if (now - g_wd_slots[i].last_advance >= timeout) {
                g_wd_slots[i].miss_count++; g_wd_slots[i].last_advance = now;
                if (g_wd_slots[i].miss_count >= g_wd_slots[i].max_miss) {
                    log_error("watchdog: %s STUCK (missed %d/%d, beat=%d)",
                        g_wd_slots[i].name, g_wd_slots[i].miss_count,
                        g_wd_slots[i].max_miss, cur);
                    /* 优雅退出：通知主循环与各工作线程结束，尽量在清理窗口内落盘和关闭资源；
                       若仍然卡死，最终由 systemd Restart=on-failure 兜底重启 */
                    app->running = 0;
                    sync();
                    usleep(200000);   /* 给健康线程 200ms 清理窗口（日志落盘 / 设备关闭） */
                    break;
                }
                else log_error("watchdog: %s timeout #%d/%d", g_wd_slots[i].name, g_wd_slots[i].miss_count, g_wd_slots[i].max_miss);
            }
        }
        if (wd_notify_sec > 0 && now >= next_notify) {
            if (sd_notify(0, "WATCHDOG=1") > 0) notify_fail = 0;
            else if (++notify_fail >= 3) { log_error("watchdog: sd_notify failed, exiting"); app->running = 0; break; }
            next_notify = now + wd_notify_sec;
        }
    }
    log_info("watchdog stopped"); return NULL;
}

void watchdog_feed(int slot) { if (slot >= 0 && slot < WD_MAX_SLOTS) __atomic_add_fetch(&g_wd_slots[slot].beat, 1, __ATOMIC_RELAXED); }
