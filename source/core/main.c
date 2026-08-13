#define _GNU_SOURCE

/**
 * main.c — 线程表框架：{ name, mod_init_t, mod_dtor_t, thread, thread_arg }
 */

#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>

#include "core/config.h"
#include "core/data_flow.h"
#include "can/can_socket.h"
#include "net/tcp_server.h"
#include "http/http.h"
#include "watchdog/watchdog.h"
#include "core/log.h"
#include "core/common.h"
#include "video/video_stream.h"

typedef struct {
    const char *wd_name;
    const char *os_name;
    pthread_t    tid;
    int  (*mod_init_t)(void *arg);
    void (*mod_dtor_t)(void *arg);
    void       *(*thread)(void *);
    int timeout;
    int max_miss;
} module_t;

static void sig_handler(int sig);

static void set_current_thread_name(const char *name)
{
    if (!name || !*name) return;
    char short_name[16];
    safe_strncpy(short_name, sizeof(short_name), name);
    (void)pthread_setname_np(pthread_self(), short_name);
}

static int signal_setup(void *arg)
{
    (void)arg;
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    /* 写已断开 socket 会触发 SIGPIPE，默认会终止进程；统一忽略，
       由 write() 的返回值处理错误（见 tcp_server.c / http.c） */
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

/* 文件级应用上下文（仅本文件直接访问；其他模块通过指针参数获得） */
static app_ctx_t g_app;

static module_t g_mods[] = {
    { "can",      "can",      0, can_init,          can_cleanup,           can_task,         3, 3 },
    { "tcp",      "tcp",      0, tcp_init,          tcp_cleanup,           tcp_task,         5, 3 },
    { "http",     "http",     0, http_server_start, http_server_stop,      http_server_task, 5, 3 },
    { "video",    "video",    0, video_stream_init, video_stream_shutdown, video_stream_task, 5, 3 },
    /* watchdog 线程是监控者，不能监督自己，故 timeout=0（不注册自身） */
    { "watchdog", "watchdog", 0, watchdog_init,     NULL,                  watchdog_task,    0, 0 },
    { "signal",   "signal",   0, signal_setup,      NULL,                  NULL,             0, 0 },
};
#define MOD_COUNT (int)(sizeof(g_mods)/sizeof(g_mods[0]))

/**
 * sig_handler - 处理退出信号，通知主循环停止服务。
 * @sig: 接收到的信号编号。
 */
static void sig_handler(int sig) { (void)sig; g_app.running = 0; }

/**
 * thread_wrapper - 线程入口函数，执行模块线程并在退出时递减活跃线程计数。
 * @arg: 指向模块描述符的指针。
 */
static void *thread_wrapper(void *arg)
{
    module_t *m = (module_t *)arg;
    set_current_thread_name(m->os_name);
    if (m->thread) m->thread(&g_app);
    watchdog_unregister_self(m->wd_name);
    __atomic_fetch_sub(&g_app.threads_running, 1, __ATOMIC_RELEASE);
    return NULL;
}

/**
 * wait_threads_exit - 在有限时间内等待所有工作线程退出，避免析构与线程并发访问资源。
 */
static void wait_threads_exit(void)
{
    for (int i = 0; i < 50; i++) {
        if (__atomic_load_n(&g_app.threads_running, __ATOMIC_ACQUIRE) <= 0) break;
        usleep(100000);   /* 每次 100ms，最多 5s */
    }
}

/**
 * shutdown_modules_safe - 安全关闭各模块并等待线程退出，避免在关闭时出现竞争或卡死。
 */
static void shutdown_modules_safe(void)
{
    log_info("Shutting down modules...");
    for (int i = 0; i < MOD_COUNT; i++) {
        module_t *m = &g_mods[i];
        if (m->mod_dtor_t) {
            log_info("Shutting down module %s...", m->wd_name);
            m->mod_dtor_t(&g_app);
        }
    }
    log_info("Modules shut down successfully.");
    g_app.running = 0;
    http_server_stop(&g_app);
    video_stream_shutdown(&g_app);
    wait_threads_exit();
}

/**
 * main - 初始化所有模块、创建线程、进入主循环，并在退出时统一清理资源。
 */
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

    /* 注册数据流虚函数实现（默认各域数据流独立、不做桥接；业务可整体替换或逐项覆盖） */
    data_flow_register(&g_app, data_flow_default_ops());
    set_current_thread_name("main");

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
            if (m->thread && m->timeout > 0)
                watchdog_register_thread(m->tid, m->wd_name, m->timeout, m->max_miss);
        }
    }

    watchdog_register_thread(pthread_self(), "main", 15, 1);
    while (g_app.running) { watchdog_feed_self("main"); sleep(1); }
    watchdog_unregister_self("main");

    /* 通知各模块停止，让工作线程尽快退出 */
    shutdown_modules_safe();

    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_mods[i].mod_dtor_t) g_mods[i].mod_dtor_t(&g_app);
    log_close(); return 0;

fail:
    watchdog_unregister_self("main");
    shutdown_modules_safe();
    for (int i = MOD_COUNT-1; i >= 0; i--)
        if (g_mods[i].mod_dtor_t) g_mods[i].mod_dtor_t(&g_app);
    log_close(); return 1;
}
