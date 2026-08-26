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
    pthread_t tid;
    char   name[WD_NAME_MAX];
    int    beat, timeout, max_miss, miss_count, last_beat;
    int    active;
    time_t last_advance;
} wd_entry_t;

static wd_entry_t g_wd_slots[WD_MAX_SLOTS];
static int wd_notify_sec;
static pthread_mutex_t g_wd_mutex = PTHREAD_MUTEX_INITIALIZER;
static time_t g_feed_miss_log_at = 0;
static char g_feed_miss_name[WD_NAME_MAX];

static int watchdog_find_slot_by_tid(pthread_t tid)
{
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (g_wd_slots[i].active && pthread_equal(g_wd_slots[i].tid, tid))
            return i;
    }
    return -1;
}

static int watchdog_find_slot_by_name(const char *name)
{
    if (!name || !*name) return -1;
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (g_wd_slots[i].active && strcmp(g_wd_slots[i].name, name) == 0)
            return i;
    }
    return -1;
}

static const char *watchdog_slot_name(const wd_entry_t *slot)
{
    return (slot && slot->name[0]) ? slot->name : "(unnamed)";
}

int watchdog_register_thread(pthread_t tid, const char *name, int timeout, int max_miss)
{
    pthread_mutex_lock(&g_wd_mutex);
    int slot = watchdog_find_slot_by_tid(tid);
    if (slot >= 0) {
        safe_strncpy(g_wd_slots[slot].name, sizeof(g_wd_slots[slot].name),
                     (name && *name) ? name : "(unnamed)");
        g_wd_slots[slot].timeout = timeout > 0 ? timeout : 1;
        g_wd_slots[slot].max_miss = max_miss > 0 ? max_miss : 1;
        g_wd_slots[slot].last_advance = time(NULL);
        LOG_INFO("watchdog: thread '%s' tid=%lu updated timeout=%ds miss=%d",
                 watchdog_slot_name(&g_wd_slots[slot]), (unsigned long)tid,
                 g_wd_slots[slot].timeout, g_wd_slots[slot].max_miss);
        pthread_mutex_unlock(&g_wd_mutex);
        return slot;
    }
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (g_wd_slots[i].active) continue;
        g_wd_slots[i].tid = tid;
        safe_strncpy(g_wd_slots[i].name, sizeof(g_wd_slots[i].name),
                     (name && *name) ? name : "(unnamed)");
        g_wd_slots[i].timeout = timeout > 0 ? timeout : 1;
        g_wd_slots[i].max_miss = max_miss > 0 ? max_miss : 1;
        g_wd_slots[i].beat = 0;
        g_wd_slots[i].last_beat = 0;
        g_wd_slots[i].miss_count = 0;
        g_wd_slots[i].last_advance = time(NULL);
        g_wd_slots[i].active = 1;
        LOG_INFO("watchdog: thread '%s' tid=%lu registered timeout=%ds miss=%d",
                 watchdog_slot_name(&g_wd_slots[i]), (unsigned long)tid,
                 g_wd_slots[i].timeout, g_wd_slots[i].max_miss);
        pthread_mutex_unlock(&g_wd_mutex);
        return i;
    }
    LOG_ERROR("watchdog: register failed for thread '%s' tid=%lu (slot full)",
              (name && *name) ? name : "(unnamed)", (unsigned long)tid);
    pthread_mutex_unlock(&g_wd_mutex);
    return -1;
}

int watchdog_unregister_thread(pthread_t tid, const char *name)
{
    pthread_mutex_lock(&g_wd_mutex);
    int slot = watchdog_find_slot_by_tid(tid);
    if (slot < 0 && name && *name)
        slot = watchdog_find_slot_by_name(name);
    if (slot < 0) {
        pthread_mutex_unlock(&g_wd_mutex);
        return -1;
    }
    LOG_INFO("watchdog: thread '%s' tid=%lu unregistered",
             watchdog_slot_name(&g_wd_slots[slot]),
             (unsigned long)g_wd_slots[slot].tid);
    g_wd_slots[slot].active = 0;
    g_wd_slots[slot].tid = (pthread_t)0;
    g_wd_slots[slot].name[0] = '\0';
    g_wd_slots[slot].beat = 0;
    g_wd_slots[slot].last_beat = 0;
    g_wd_slots[slot].timeout = 0;
    g_wd_slots[slot].max_miss = 0;
    g_wd_slots[slot].miss_count = 0;
    g_wd_slots[slot].last_advance = 0;
    pthread_mutex_unlock(&g_wd_mutex);
    return 0;
}

int watchdog_feed_thread(pthread_t tid, const char *name)
{
    pthread_mutex_lock(&g_wd_mutex);
    int slot = watchdog_find_slot_by_tid(tid);
    if (slot < 0 && name && *name)
        slot = watchdog_find_slot_by_name(name);
    if (slot < 0) {
        time_t now = time(NULL);
        const char *miss_name = (name && *name) ? name : "(unnamed)";
        if (now != g_feed_miss_log_at || strcmp(g_feed_miss_name, miss_name) != 0) {
            g_feed_miss_log_at = now;
            safe_strncpy(g_feed_miss_name, sizeof(g_feed_miss_name), miss_name);
            LOG_ERROR("watchdog: feed ignored for unregistered thread '%s' tid=%lu",
                      miss_name, (unsigned long)tid);
        }
        pthread_mutex_unlock(&g_wd_mutex);
        return -1;
    }
    /* 槽位名称在注册时确定，feed 只是喂心跳，不得改写名称：
       名称用于日志与按名查找，若允许 feed 改写，误传名称会污染注册信息 */
    __atomic_add_fetch(&g_wd_slots[slot].beat, 1, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_wd_mutex);
    return slot;
}

int watchdog_init(void *arg)
{
    (void)arg;
    const char *env_usec = getenv("WATCHDOG_USEC");
    if (env_usec) { long usec = atol(env_usec); if (usec > 0) { wd_notify_sec = (int)(usec / 2000000); if (wd_notify_sec < 1) wd_notify_sec = 1; LOG_INFO("watchdog: systemd WATCHDOG_USEC=%ld notify=%ds", usec, wd_notify_sec); } }
    return 0;
}

void *watchdog_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    time_t now = time(NULL);
    pthread_mutex_lock(&g_wd_mutex);
    for (int i = 0; i < WD_MAX_SLOTS; i++) {
        if (!g_wd_slots[i].active) continue;
        g_wd_slots[i].last_beat = __atomic_load_n(&g_wd_slots[i].beat, __ATOMIC_RELAXED);
        g_wd_slots[i].last_advance = now;
    }
    pthread_mutex_unlock(&g_wd_mutex);
    LOG_INFO("watchdog started");
    sd_notify(0, "READY=1");
    time_t next_notify = 0; int notify_fail = 0;
    if (wd_notify_sec > 0) { sd_notify(0, "WATCHDOG=1"); next_notify = time(NULL) + wd_notify_sec; }
    while (app->running) {
        sleep(1); now = time(NULL);
        int should_stop = 0;

        pthread_mutex_lock(&g_wd_mutex);
        for (int i = 0; i < WD_MAX_SLOTS; i++) {
            if (!g_wd_slots[i].active) continue;
            int timeout = g_wd_slots[i].timeout; if (timeout <= 0) continue;
            int cur = __atomic_load_n(&g_wd_slots[i].beat, __ATOMIC_RELAXED);
            if (cur != g_wd_slots[i].last_beat) { g_wd_slots[i].last_beat = cur; g_wd_slots[i].last_advance = now; g_wd_slots[i].miss_count = 0; }
            else if (now - g_wd_slots[i].last_advance >= timeout) {
                g_wd_slots[i].miss_count++; g_wd_slots[i].last_advance = now;
                if (g_wd_slots[i].miss_count >= g_wd_slots[i].max_miss) {
                    LOG_ERROR("watchdog: thread '%s' tid=%lu STUCK (missed %d/%d, beat=%d)",
                        watchdog_slot_name(&g_wd_slots[i]),
                        (unsigned long)g_wd_slots[i].tid, g_wd_slots[i].miss_count,
                        g_wd_slots[i].max_miss, cur);
                    /* 优雅退出：通知主循环与各工作线程结束，尽量在清理窗口内落盘和关闭资源；
                       若仍然卡死，最终由 systemd Restart=on-failure 兜底重启 */
                    should_stop = 1;
                    break;
                }
                else LOG_ERROR("watchdog: thread '%s' tid=%lu timeout #%d/%d",
                               watchdog_slot_name(&g_wd_slots[i]),
                               (unsigned long)g_wd_slots[i].tid,
                               g_wd_slots[i].miss_count, g_wd_slots[i].max_miss);
            }
        }
        pthread_mutex_unlock(&g_wd_mutex);

        if (should_stop) {
            app->running = 0;
            sync();
            usleep(200000);   /* 给健康线程 200ms 清理窗口（日志落盘 / 设备关闭） */
            break;
        }

        if (wd_notify_sec > 0 && now >= next_notify) {
            if (sd_notify(0, "WATCHDOG=1") > 0) notify_fail = 0;
            else if (++notify_fail >= 3) { LOG_ERROR("watchdog: sd_notify failed, exiting"); app->running = 0; break; }
            next_notify = now + wd_notify_sec;
        }
    }
    LOG_INFO("watchdog stopped"); return NULL;
}
