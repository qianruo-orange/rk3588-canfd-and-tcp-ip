/**
 * main.c — 线程表框架：{ name, mod_init_t, mod_dtor_t, thread, thread_arg }
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "core/config.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "video/video_stream.h"

typedef struct {
    const char  *name;
    int  (*mod_init_t)(void *arg);
    void (*mod_dtor_t)(void *arg);
    void       *(*thread)(void *);
    pthread_t    tid;
} module_t;

static void sig_handler(int sig);

static int signal_setup(void *arg)
{
    (void)arg;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    /* 写已断开 socket 会触发 SIGPIPE，默认会终止进程；统一忽略，
       由 write() 的返回值处理错误（见 send_data.c / http.c） */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* 文件级应用上下文（仅本文件直接访问；其他模块通过指针参数获得） */
static app_ctx_t g_app;

static module_t g_mods[] = {
    { "can",      can_init,           can_cleanup,            can_task },
    { "tcp",      tcp_init,           tcp_cleanup,            tcp_task },
    { "http",     http_server_start,  http_server_stop,       http_server_task },
    { "video",    video_stream_init,  video_stream_shutdown,  video_stream_task },
    { "watchdog", watchdog_init,      NULL,                   watchdog_task },
    { "signal",   signal_setup,       NULL,                   NULL },
};
#define MOD_COUNT (int)(sizeof(g_mods)/sizeof(g_mods[0]))

static void sig_handler(int sig) { (void)sig; g_app.running = 0; }

/* 线程包装器：线程退出时递减活跃计数，供主线程有界等待 */
static void *thread_wrapper(void *arg)
{
    module_t *m = (module_t *)arg;
    if (m->thread) m->thread(&g_app);
    __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELEASE);
    return NULL;
}

/* 有界等待工作线程退出：避免 dtor 与线程并发访问资源；
   卡死线程无法等待时由 systemd WatchdogSec 兜底重启 */
static void wait_threads_exit(void)
{
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&g_app.threads_running, __ATOMIC_ACQUIRE) <= 0) break;
        usleep(100000);   /* 每次 100ms，最多 5s */
    }
}

int main(void)
{
    can_ctx_t           can_ctx;
    tcp_ctx_t           tcp_ctx;
    struct app_config_t cfg;

    memset(&g_app, 0, sizeof(g_app));
    g_app.running = 1;
    pthread_mutex_init(&g_app.can_mutex, NULL);
    g_app.can = &can_ctx;
    g_app.tcp = &tcp_ctx;
    g_app.cfg = &cfg;

    log_init(PATH_LOGS);
    config_load(&cfg);
    log_close(); log_init(cfg.log_dir);

    signal(SIGINT, sig_handler); signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    for (int i = 0; i < MOD_COUNT; i++) {
        module_t *m = &g_mods[i];
        if (m->mod_init_t && m->mod_init_t(&g_app) < 0) goto fail;
        if (m->thread) {
            /* 以 detached 模式创建线程：不回收、不 join，
               避免卡死线程导致 pthread_join 永久阻塞 */
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            __atomic_fetch_add(&g_app.threads_running, 1, __ATOMIC_RELAXED);
            int rc = pthread_create(&m->tid, &attr, thread_wrapper, m);
            pthread_attr_destroy(&attr);
            if (rc != 0) goto fail;
        }
    }

    while (g_app.running) { watchdog_feed(WD_MAIN); sleep(1); }

    /* 通知各模块停止，让工作线程尽快退出 */
    http_server_stop(&g_app); video_stream_shutdown(&g_app);
    /* 有界等待线程退出后再执行 dtor，避免与线程并发访问资源 */
    wait_threads_exit();

    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_mods[i].mod_dtor_t) g_mods[i].mod_dtor_t(&g_app);
    log_close(); return 0;

fail:
    http_server_stop(&g_app); video_stream_shutdown(&g_app);
    wait_threads_exit();
    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_mods[i].mod_dtor_t) g_mods[i].mod_dtor_t(&g_app);
    log_close(); return 1;
}
