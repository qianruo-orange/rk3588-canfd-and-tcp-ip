/**
 * http.c — HTTP 核心：epoll Reactor 主循环、路由分发、认证、静态文件服务、工具函数。
 *
 * 架构说明（Reactor 单线程，无"每客户端一线程"）：
 *  - 单个 epoll 实例驱动监听 socket 与全部客户端 socket，所有 IO 均为非阻塞；
 *  - 每个连接是一个状态机：读请求（累积缓冲 → 收齐 header+body）→ 处理分发
 *    → 排空输出缓冲 → 关闭；慢客户端只会让自己连接的输出缓冲增长，不会阻塞
 *    主循环，也不会阻塞其他连接；
 *  - 输出缓冲有上限 HTTP_WBUF_MAX，超限直接断开，防止慢客户端耗尽内存；
 *  - 响应输出统一走 http_send_response（小响应）与 http_serve_stream（流式
 *    数据源：静态文件 / 日志下载 / 日志打包共用同一套逐块填充+排空逻辑）。
 */

#define _GNU_SOURCE   /* 暴露 GNU/Linux 扩展 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <shadow.h>
#include <crypt.h>
#include <pwd.h>
#include <grp.h>

#include "http/http_internal.h"
#include "video/video_stream.h"
#include "watchdog/watchdog.h"
#include "core/common.h"
#include "core/epoll_util.h"

/* ---- 全局变量 ---- */
static int g_listen_fd = -1;
static int g_epfd = -1;              /* 供连接关闭时摘除 epoll 条目（单线程主循环持有） */
static app_ctx_t *g_http_app = NULL; /* http_server_task 持有的应用上下文 */
/* 保护 g_listen_fd 的关闭操作：http_server_task 退出段与 http_server_stop
   （watchdog 强杀时可能并发）都需要安全关闭，避免 double-close */
static pthread_mutex_t g_listen_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_http_active = 0;   /* 活跃 HTTP 连接数 */
#define HTTP_MAX_CONN 64

typedef void (*api_fn)(app_ctx_t *, int, const char *, const char *, const char *);

/* ---- API 包装函数：统一 api_fn 签名，转发到各具体实现 ----
   只接收 (app, fd) 的接口用宏批量生成（fn 形如 http_xxx，生成 http_xxx_wrap）；
   需要访问 method/uri/req 的接口（toggle / dbc_upload / send）单独手写 */
#define WRAP_API_NOBODY(fn) \
    static void fn##_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) \
    { (void)method; (void)uri; (void)req; fn(app, fd); }

WRAP_API_NOBODY(http_system_api)
WRAP_API_NOBODY(http_can_status)
WRAP_API_NOBODY(http_can_ifaces)
WRAP_API_NOBODY(http_can_decoded)
WRAP_API_NOBODY(http_can_decoded_tx)
WRAP_API_NOBODY(http_can_rx)
WRAP_API_NOBODY(http_reboot)
WRAP_API_NOBODY(http_shutdown)
WRAP_API_NOBODY(http_network_api)

static void http_can_toggle_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; http_can_toggle(app, fd, req); }
static void http_can_dbc_upload_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { http_can_dbc_upload(app, fd, method, uri, req); }
static void http_can_send_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { http_can_send(app, fd, method, uri, req); }

#undef WRAP_API_NOBODY

/* ---- 每连接状态机（Reactor 单线程） ---- */
#define HTTP_WBUF_INIT 8192             /* 输出缓冲初始大小 */
#define HTTP_WBUF_MAX  (16UL * 1024 * 1024)  /* 输出缓冲上限：慢客户端超限即断开 */
#define HTTP_BODY_MAX  ((long)HTTP_BUF_SIZE - 4096)  /* 请求 body 上限（须能收进读缓冲） */
#define HTTP_UPLOAD_MAX (64L * 1024 * 1024)  /* /api/ai/upload body 上限（落盘，不占读缓冲） */
#define HTTP_CONN_IDLE 30               /* 连接空闲超时（秒） */

typedef struct http_conn {
    int fd;
    struct http_conn *next;

    /* 读缓冲 */
    char rbuf[HTTP_BUF_SIZE];
    size_t rlen;

    /* 请求解析结果 */
    int hdr_done;          /* 已收到完整请求头 */
    size_t hdr_len;        /* 请求头长度（含结尾分隔符） */
    long body_cl;          /* Content-Length；-1 表示已被拒绝 */
    char method[16];
    char uri[HTTP_URI_MAX];

    /* AI 文件上传（/api/ai/upload）：body 流式落盘到临时文件，不占读缓冲 */
    FILE *up_fp;           /* 上传临时文件（NULL = 非上传连接） */
    char up_path[64];      /* 上传临时文件路径（fd 在连接生命周期内唯一，不冲突） */
    long up_cl;            /* 上传 Content-Length */
    long up_got;           /* 已落盘字节数 */

    /* 输出缓冲 */
    char *wbuf;
    size_t wcap, wlen, woff;

    /* 流式数据源（文件）：http_serve_stream 统一管理，发送完/关闭时 fclose，
       若设置了 src_unlink 同时删除该临时文件 */
    FILE *src;
    size_t src_remain;
    int src_eof;
    char *src_unlink;

    int done;              /* 请求已处理，不再接收更多数据 */
    int closed;
    time_t last_active;
} http_conn_t;

static http_conn_t *g_conns = NULL;    /* 活动连接链表（单线程主循环访问，无需锁） */
static http_conn_t *g_dead = NULL;     /* 已关闭连接的残留结构体（延迟释放，防 use-after-free） */

static http_conn_t *conn_find(int fd)
{
    for (http_conn_t *c = g_conns; c; c = c->next)
        if (c->fd == fd) return c;
    return NULL;
}

/* 释放已关闭连接的残留结构体：必须放在一轮事件处理全部结束之后调用，
   因为调用方（conn_read/process_request/主循环）在 conn_close 后仍可能
   读取 c->closed 等字段判断状态 */
static void conn_reap(void)
{
    while (g_dead) {
        http_conn_t *d = g_dead;
        g_dead = d->next;
        free(d);
    }
}

/* 关闭连接：摘除 epoll、从链表移除、释放源/缓冲/关闭 fd 并归还连接计数；
   结构体延迟释放（挂 g_dead），由主循环 conn_reap 统一 free */
static void conn_close(http_conn_t *c)
{
    if (!c || c->closed) return;
    c->closed = 1;
    if (g_epfd >= 0)
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    http_conn_t **pp = &g_conns;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    if (c->src) fclose(c->src);
    if (c->src_unlink) { unlink(c->src_unlink); free(c->src_unlink); c->src_unlink = NULL; }
    if (c->up_fp) { fclose(c->up_fp); c->up_fp = NULL; }
    if (c->up_path[0]) { unlink(c->up_path); c->up_path[0] = 0; }
    close(c->fd);
    c->fd = -1;
    free(c->wbuf);
    c->wbuf = NULL;
    c->next = g_dead;
    g_dead = c;
    __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
}

/* 把 fd 移交给视频推流线程：从链表移除、释放缓冲，但不 close fd，
   也不归还连接计数（计数由 video 线程退出回调 http_video_client_closed 归还）；
   结构体同样延迟释放 */
static void conn_detach(http_conn_t *c)
{
    if (g_epfd >= 0)
        epoll_ctl(g_epfd, EPOLL_CTL_DEL, c->fd, NULL);
    http_conn_t **pp = &g_conns;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    c->closed = 1;
    if (c->src) fclose(c->src);
    if (c->src_unlink) { unlink(c->src_unlink); free(c->src_unlink); }
    if (c->up_fp) { fclose(c->up_fp); c->up_fp = NULL; }
    if (c->up_path[0]) { unlink(c->up_path); c->up_path[0] = 0; }
    free(c->wbuf);
    c->wbuf = NULL;
    c->next = g_dead;
    g_dead = c;
}

static http_conn_t *conn_new(int fd)
{
    http_conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->wbuf = malloc(HTTP_WBUF_INIT);
    if (!c->wbuf) { free(c); return NULL; }
    c->fd = fd;
    c->wcap = HTTP_WBUF_INIT;
    c->last_active = time(NULL);
    c->next = g_conns;
    g_conns = c;
    return c;
}

/* AI 文件上传走落盘模式：body 直接写入临时文件，不占读缓冲 */
static int conn_is_ai_upload(const http_conn_t *c)
{
    return strncmp(c->uri, "/api/ai/upload", 15) == 0;
}

/* 追加数据到输出缓冲；超限（慢客户端）直接断开连接 */
static int conn_append(http_conn_t *c, const void *data, size_t len)
{
    if (c->closed || len == 0) return 0;
    if (c->wlen + len > HTTP_WBUF_MAX) {
        conn_close(c);
        return -1;
    }
    if (c->wlen + len > c->wcap) {
        size_t ncap = c->wcap * 2;
        while (ncap < c->wlen + len) ncap *= 2;
        char *nb = realloc(c->wbuf, ncap);
        if (!nb) { conn_close(c); return -1; }
        c->wbuf = nb;
        c->wcap = ncap;
    }
    memcpy(c->wbuf + c->wlen, data, len);
    c->wlen += len;
    return 0;
}

/* 从流式数据源填充输出缓冲（每次填满 wcap），源读完后关闭并回收 */
static void src_fill(http_conn_t *c)
{
    if (!c->src || c->src_eof) return;
    while (c->wlen < c->wcap && c->src_remain > 0) {
        size_t want = c->wcap - c->wlen;
        if (want > c->src_remain) want = c->src_remain;
        size_t r = fread(c->wbuf + c->wlen, 1, want, c->src);
        if (r == 0) { conn_close(c); return; }   /* 读源失败：中止连接 */
        c->wlen += r;
        c->src_remain -= r;
    }
    if (c->src_remain == 0) {
        fclose(c->src);
        c->src = NULL;
        c->src_eof = 1;
        if (c->src_unlink) { unlink(c->src_unlink); free(c->src_unlink); c->src_unlink = NULL; }
    }
}

/* 非阻塞排空输出缓冲；排空后若仍有流式源则续填，全部完成则关闭连接。
   每个事件最多写 1MB，避免单个连接独占主循环 */
static void conn_drain(http_conn_t *c)
{
    int wbytes = 0;
    while (c->woff < c->wlen) {
        ssize_t w = write(c->fd, c->wbuf + c->woff, c->wlen - c->woff);
        if (w > 0) {
            c->woff += (size_t)w;
            wbytes += (int)w;
            c->last_active = time(NULL);
            if (wbytes >= 1024 * 1024) break;
            continue;
        }
        if (w < 0 && errno == EINTR) continue;
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        conn_close(c);
        return;
    }
    if (c->closed) return;

    if (c->woff >= c->wlen) {
        /* 输出缓冲已排空 */
        c->wlen = c->woff = 0;
        if (c->src) {
            src_fill(c);
            if (c->closed) return;
            if (c->wlen > 0) { conn_drain(c); return; }
            if (!c->src) { conn_close(c); return; }   /* 源读完：连接结束 */
            return;
        }
        conn_close(c);   /* 响应发送完成 */
    }
}

/* 同步当前 epoll 关注事件：输出缓冲非空时监听 EPOLLOUT */
static void conn_update_events(http_conn_t *c)
{
    if (c->closed || g_epfd < 0) return;
    uint32_t e = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
    if (c->woff < c->wlen) e |= EPOLLOUT;
    struct epoll_event ev = { .events = e, .data.fd = c->fd };
    epoll_ctl(g_epfd, EPOLL_CTL_MOD, c->fd, &ev);
}

/* ---- 工具函数 ---- */

/**
 * http_mime_type - 根据文件扩展名返回对应的 HTTP Content-Type。
 * @path: 目标资源路径。
 * @return: MIME 类型字符串。
 */
static const char *http_mime_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (strcmp(ext, ".css")  == 0) return "text/css";
    if (strcmp(ext, ".js")   == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png")  == 0) return "image/png";
    if (strcmp(ext, ".ico")  == 0) return "image/x-icon";
    if (strcmp(ext, ".svg")  == 0) return "image/svg+xml";
    return "text/plain";
}

/* 非连接上下文（如视频推流线程等）的完整写入统一走 core/common.h 的
   fd_write_all_blocking；连接处理一律走 http_send_response / http_serve_stream
   的非阻塞输出缓冲 */

/**
 * http_write_all - 完整写入客户端 socket。
 * 连接上下文内：追加到输出缓冲（非阻塞，由主循环排空）；连接上下文外（如
 * 视频线程）：退化为阻塞写。@return 成功 0，失败 -1。
 */
int http_write_all(int fd, const void *data, size_t len)
{
    http_conn_t *c = conn_find(fd);
    if (c) return conn_append(c, data, len) == 0 ? 0 : -1;
    return fd_write_all_blocking(fd, data, len);
}

/**
 * http_send_response - 向客户端发送标准 HTTP 响应头和响应体（追加到连接输出缓冲）。
 */
void http_send_response(int fd, int code, const char *status,
                        const char *mime, const void *body, size_t len)
{
    char header[512];
    int off = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "%s"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Cache-Control: no-cache\r\n"
                       "Connection: close\r\n\r\n",
                       code, status,
                       code == 401 ? "WWW-Authenticate: Basic realm=\"data_transport\"\r\n" : "",
                       mime, len);
    if (off < 0) return;
    http_conn_t *c = conn_find(fd);
    if (!c) {   /* 非连接上下文：阻塞发送兜底 */
        fd_write_all_blocking(fd, header, (size_t)off);
        if (body && len > 0) fd_write_all_blocking(fd, body, len);
        return;
    }
    conn_append(c, header, (size_t)off);
    if (body && len > 0) conn_append(c, body, len);
}

/**
 * http_serve_stream - 流式发送数据源到客户端（静态文件/日志下载/日志打包共用）。
 * 服务器接管 src 所有权，发送完毕或连接关闭时负责 fclose；若 unlink_after
 * 非空，发送完成后删除该文件（用于临时打包文件）。
 */
void http_serve_stream(int fd, const char *mime, const char *extra_hdr,
                       FILE *src, size_t size, const char *unlink_after)
{
    http_conn_t *c = conn_find(fd);
    if (!c) { fclose(src); return; }
    char header[1024];
    int off = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "%s"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       mime, extra_hdr ? extra_hdr : "", size);
    if (off > 0) conn_append(c, header, (size_t)off);
    c->src = src;
    c->src_remain = size;
    c->src_eof = 0;
    if (unlink_after) {
        free(c->src_unlink);
        c->src_unlink = strdup(unlink_after);
    }
}

/**
 * http_handle_404 - 处理未找到的静态资源请求，并返回 404 页面。
 */
void http_handle_404(int fd, const char *path)
{
    char msg[256];
    snprintf(msg, sizeof(msg),
             "<html><body><h1>404 Not Found</h1><p>%s</p></body></html>", path);
    http_send_response(fd, 404, "Not Found", "text/html", msg, strlen(msg));
}

/* ---- HTTP Basic 认证 ---- */

static pthread_mutex_t g_auth_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 认证暴力破解限速：按来源 IP 计数。
   全局熔断会被匿名攻击者刷满失败计数从而 DoS 合法用户，因此按 IP 隔离，
   仅封禁肇事 IP；getpwnam/getspnam/crypt 本身也由 g_auth_mutex 串行保护 */
#define AUTH_FAIL_WINDOW 10   /* 窗口：10 秒 */
#define AUTH_FAIL_LIMIT  20   /* 窗口内允许的最大失败次数 */
#define AUTH_IP_TABLE    64
typedef struct { char ip[INET_ADDRSTRLEN]; int fail; time_t win; } auth_fail_t;
static auth_fail_t g_auth_fail[AUTH_IP_TABLE];

/**
 * http_check_auth_common - 采用 Basic Auth 进行认证，支持普通用户和 root 用户要求。
 * @req: HTTP 请求头。
 * @fd: 客户端 socket。
 * @require_root: 是否要求 root 权限。
 * @return: 认证成功返回 1，否则返回 0。
 */
static int http_check_auth_common(const char *req, int fd, int require_root)
{
    /* 取来源 IP 作为限速 key */
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    char ip[INET_ADDRSTRLEN] = "unknown";
    if (getpeername(fd, (struct sockaddr *)&peer, &plen) == 0)
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

    /* 暴力破解限速：窗口内该 IP 失败超限直接拒绝（不执行 crypt，避免 CPU DoS） */
    time_t now = time(NULL);
    pthread_mutex_lock(&g_auth_mutex);
    int slot = -1, free_slot = -1;
    for (int i = 0; i < AUTH_IP_TABLE; i++) {
        if (g_auth_fail[i].ip[0] && strcmp(g_auth_fail[i].ip, ip) == 0) { slot = i; break; }
        if (!g_auth_fail[i].ip[0] && free_slot < 0) free_slot = i;
    }
    if (slot < 0) {
        if (free_slot < 0) {   /* 表满：淘汰最久未更新的条目 */
            free_slot = 0;
            for (int i = 1; i < AUTH_IP_TABLE; i++)
                if (g_auth_fail[i].win < g_auth_fail[free_slot].win) free_slot = i;
        }
        slot = free_slot;
        safe_strncpy(g_auth_fail[slot].ip, sizeof(g_auth_fail[slot].ip), ip);
        g_auth_fail[slot].fail = 0;
        g_auth_fail[slot].win  = now;
    }
    if (now - g_auth_fail[slot].win >= AUTH_FAIL_WINDOW) {
        g_auth_fail[slot].fail = 0;
        g_auth_fail[slot].win  = now;
    }
    int rate_limited = (g_auth_fail[slot].fail >= AUTH_FAIL_LIMIT);
    pthread_mutex_unlock(&g_auth_mutex);

    if (rate_limited) {
        LOG_INFO("HTTP auth: rate limited from %s", ip);
        goto deny;
    }

    /* HTTP 头不区分大小写，用 strcasestr 定位认证头 */
    const char *auth = strcasestr(req, "Authorization: Basic ");
    if (!auth) goto deny_nocount;   /* 未携带凭据只拒绝，不计失败数：
        前端未登录时的请求会频繁 401，若计数会把本机 IP 自己锁进限速 */

    char decoded[128] = {0};
    const char *enc = auth + 21;
    int di = 0;
    for (int i = 0; enc[i] && enc[i] != '\r' && enc[i] != '\n' && di < 120; i++) {
        if (enc[i] >= 'A' && enc[i] <= 'Z') decoded[di] = (char)(enc[i] - 'A');
        else if (enc[i] >= 'a' && enc[i] <= 'z') decoded[di] = (char)(enc[i] - 'a' + 26);
        else if (enc[i] >= '0' && enc[i] <= '9') decoded[di] = (char)(enc[i] - '0' + 52);
        else if (enc[i] == '+') decoded[di] = 62;
        else if (enc[i] == '/') decoded[di] = 63;
        else if (enc[i] == '=') break;   /* Base64 填充符，其后不再有有效数据 */
        else goto deny;   /* 非法 Base64 字符：直接拒绝，不宽松跳过 */
        ++di;
    }
    if (di == 0 || di % 4 == 1) goto deny;   /* 空凭据或非法 Base64 长度 */

    char user_pass[128];
    int up_len = 0;
    for (int i = 0; i < di; i += 4) {
        if (up_len + 3 >= (int)sizeof(user_pass)) break;
        user_pass[up_len++] = (char)((decoded[i] << 2) | (decoded[i+1] >> 4));
        if (i + 2 < di)
            user_pass[up_len++] = (char)((decoded[i+1] << 4) | (decoded[i+2] >> 2));
        if (i + 3 < di)
            user_pass[up_len++] = (char)((decoded[i+2] << 6) | decoded[i+3]);
    }
    user_pass[up_len] = '\0';

    char *colon = strchr(user_pass, ':');
    if (!colon) goto deny;
    *colon = '\0';
    char *username = user_pass;
    char *password = colon + 1;

    /* getpwnam/getspnam 使用进程级静态缓冲区，多线程并发会互相覆盖；
       整个认证流程（查账号 + 校验哈希）加锁串行化 */
    pthread_mutex_lock(&g_auth_mutex);
    struct passwd *pw = getpwnam(username);
    if (!pw) goto auth_unlock_deny;

    struct spwd *sp = getspnam(username);
    if (!sp) goto auth_unlock_deny;

    char *hash = sp->sp_pwdp;
    if (!hash || hash[0] == '!' || hash[0] == '*') goto auth_unlock_deny;

    char *result = crypt(password, hash);
    int ok = (result && strcmp(result, hash) == 0);
    if (ok && require_root && pw->pw_uid != 0) {
        /* 管理权限 = root 或 sudo 组成员（板卡默认锁定 root 密码，
           实际管理员为 orangepi 等 sudo 用户；getgrouplist 返回 -1 时
           已尽量填充缓冲区，按实际容量截断检查） */
        gid_t groups[32];
        int ngroups = (int)(sizeof(groups) / sizeof(groups[0]));
        if (getgrouplist(username, pw->pw_gid, groups, &ngroups) < 0)
            ngroups = (int)(sizeof(groups) / sizeof(groups[0]));
        ok = 0;
        for (int i = 0; i < ngroups; i++) {
            struct group *gr = getgrgid(groups[i]);
            if (gr && strcmp(gr->gr_name, "sudo") == 0) { ok = 1; break; }
        }
    }
    pthread_mutex_unlock(&g_auth_mutex);

    if (ok) {
        /* 认证成功：清除该 IP 的失败计数 */
        pthread_mutex_lock(&g_auth_mutex);
        g_auth_fail[slot].fail = 0;
        g_auth_fail[slot].win  = now;
        pthread_mutex_unlock(&g_auth_mutex);
        return 1;
    }

    LOG_INFO("HTTP auth: user '%s' denied", username);
    goto deny;

auth_unlock_deny:
    pthread_mutex_unlock(&g_auth_mutex);
    LOG_INFO("HTTP auth: user '%s' denied", username);
deny:
    pthread_mutex_lock(&g_auth_mutex);
    g_auth_fail[slot].fail++;
    pthread_mutex_unlock(&g_auth_mutex);
deny_nocount:
    http_send_response(fd, 401, "Unauthorized", "text/html", "", 0);
    return 0;
}

/**
 * http_check_auth_user - 检查普通用户权限的 Basic Auth。
 */
static int http_check_auth_user(const char *req, int fd) { return http_check_auth_common(req, fd, 0); }

/**
 * http_check_auth_root - 检查 root 权限的 Basic Auth。
 */
static int http_check_auth_root(const char *req, int fd) { return http_check_auth_common(req, fd, 1); }

/* ---- 静态文件服务 ---- */

static void http_video_client_closed(int fd);

/**
 * http_serve_file - 根据 URI 提供静态文件服务（复用 http_serve_stream 流式发送）。
 */
void http_serve_file(int fd, const char *uri)
{
    if (strstr(uri, "..")) { http_handle_404(fd, uri); return; }

    char path[512];
    if (strcmp(uri, "/") == 0)
        snprintf(path, sizeof(path), "%s/index.html", HTTP_ROOT);
    else
        snprintf(path, sizeof(path), "%s%s", HTTP_ROOT, uri);

    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, uri); return; }

    size_t size = http_file_size(fp);
    if (size > 20 * 1024 * 1024) {   /* 静态文件上限 20MB，防止大文件长时间占用连接 */
        fclose(fp);
        http_send_response(fd, 413, "Payload Too Large", "text/plain", "file too large", 15);
        return;
    }

    /* no-cache：前端文件每次部署后立即可见，避免浏览器启发式缓存旧 JS/CSS */
    http_serve_stream(fd, http_mime_type(path), "Cache-Control: no-cache\r\n", fp, size, NULL);
}

/* 视频推流线程退出回调：关闭 fd 并归还连接计数 */
static void http_video_client_closed(int fd)
{
    close(fd);
    __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
}

/* ---- 请求处理（连接状态机的一部分） ---- */

/* 视频 MJPEG 走独立推流线程（video 模块创建），其余全部在 Reactor 主循环内处理 */
static void http_process_request(http_conn_t *c)
{
    app_ctx_t *app = g_http_app;
    int fd = c->fd;
    const char *req = c->rbuf;

    /* 认证/命令类处理可能耗时（crypt 等），先喂狗 */
    watchdog_feed_self("http");

    if (strcmp(c->uri, "/video/mjpeg_ai") == 0) {
        /* 画框流：AI 可用时推画框帧，AI 降级时自动回退原始帧 */
        if (video_stream_client_start_ai(fd, http_video_client_closed) != 0) {
            http_send_response(fd, 500, "Error", "text/plain", "", 0);
        } else {
            conn_detach(c);   /* fd 移交给视频推流线程 */
            return;
        }
        goto finish;
    }

    if (strcmp(c->uri, "/video/mjpeg") == 0) {
        if (video_stream_client_start(fd, http_video_client_closed) != 0) {
            http_send_response(fd, 500, "Error", "text/plain", "", 0);
        } else {
            conn_detach(c);   /* fd 移交给视频推流线程 */
            return;
        }
        goto finish;
    }

    if (strcmp(c->uri, "/") == 0 || strcmp(c->uri, "/index.html") == 0) {
        if (!http_check_auth_user(req, fd)) goto finish;
        http_serve_file(fd, "/index.html"); goto finish;
    }
    if (strcmp(c->uri, "/config") == 0 || strncmp(c->uri, "/config.html", 11) == 0) {
        if (!http_check_auth_root(req, fd)) goto finish;
        http_serve_file(fd, "/config.html"); goto finish;
    }
    if (strcmp(c->uri, "/dbc") == 0 || strncmp(c->uri, "/dbc.html", 9) == 0) {
        if (!http_check_auth_user(req, fd)) goto finish;
        http_serve_file(fd, "/dbc.html"); goto finish;
    }
    if (strncmp(c->uri, "/api/ai/upload", 15) == 0) {
        /* AI 文件上传：body 已由读取状态机落盘到 c->up_path */
        if (!http_check_auth_root(req, fd)) goto finish;
        http_ai_upload(app, fd, c->uri, c->up_path);
        goto finish;
    }

    static const struct { const char *uri; int pre; const char *method; api_fn fn; int (*auth)(const char*,int); }
    rt[] = {
        { "/api/logs",      0, NULL,   http_logs_handler, http_check_auth_root },
        { "/api/logs/clear",0, "POST", http_logs_handler, http_check_auth_root },
        { "/logs",          1, NULL,   http_logs_handler, http_check_auth_root },
        { "/logfile/",      1, NULL,   http_logs_handler, http_check_auth_root },
        { "/api/system",    0, NULL,   http_system_api_wrap,   NULL },
        { "/api/can",       0, NULL,   http_can_status_wrap,   NULL },
        { "/api/can/ifaces",0, NULL,   http_can_ifaces_wrap,   NULL },
        { "/api/can/decoded",0,NULL,   http_can_decoded_wrap,  NULL },
        { "/api/can/decoded/tx",0,NULL,http_can_decoded_tx_wrap, NULL },
        { "/api/can/dbc",    0, "POST", http_can_dbc_upload_wrap, http_check_auth_root },
        { "/api/can/send",   0, "POST", http_can_send_wrap,   http_check_auth_root },
        { "/api/can/frames", 0, NULL,   http_can_rx_wrap,     NULL },
        { "/api/can/toggle",0, "POST", http_can_toggle_wrap, http_check_auth_root },
        { "/api/config",    0, "POST", http_config_post,  http_check_auth_root },
        { "/api/config",    0, NULL,   http_config_get,   NULL },
        { "/api/reboot",    0, NULL,   http_reboot_wrap,       http_check_auth_root },
        { "/api/shutdown",  0, NULL,   http_shutdown_wrap,     http_check_auth_root },
        { "/api/network",   0, NULL,   http_network_api_wrap,  NULL },
        { "/api/network/ifaces",0,NULL,http_network_ifaces, NULL },
        { "/api/video/caps",1, NULL,  http_video_caps,   NULL },
        { "/api/video/devices",0,NULL, http_video_devices,NULL },
        { "/api/rec/start",0, "POST", http_rec_handler,  http_check_auth_root },
        { "/api/rec/stop", 0, "POST", http_rec_handler,  http_check_auth_root },
        { "/api/rec/delete",0,"POST", http_rec_handler,  http_check_auth_root },
        { "/api/rec/clear", 0,"POST", http_rec_handler,  http_check_auth_root },
        { "/api/rec/status",0, NULL,  http_rec_handler,  NULL },
        { "/api/rec/list",  0, NULL,  http_rec_handler,  NULL },
        { "/api/rec/pack",  0, NULL,  http_rec_handler,  http_check_auth_root },
        { "/recfile/",      1, NULL,  http_rec_handler,  http_check_auth_root },
    };

    for (int i = 0; i < (int)(sizeof(rt)/sizeof(rt[0])); i++) {
        int m = rt[i].pre ? !strncmp(c->uri, rt[i].uri, strlen(rt[i].uri))
                          : !strcmp(c->uri, rt[i].uri);
        if (!m) continue;
        if (rt[i].method && strcmp(c->method, rt[i].method) != 0) continue;
        if (rt[i].auth && !rt[i].auth(req, fd)) goto finish;
        rt[i].fn(app, fd, c->method, c->uri, req);
        goto finish;
    }

    /* 兜底：静态文件（公开） */
    http_serve_file(fd, c->uri);

finish:
    /* 尝试立即排空输出（多数响应一次写完）；未写完则等 EPOLLOUT */
    if (!c->closed) {
        conn_drain(c);
        if (!c->closed) conn_update_events(c);
    }
}

/* 读事件：累积请求缓冲，收齐 header+body 后触发处理 */
static void conn_read(http_conn_t *c)
{
    if (c->done) return;
    if (c->body_cl < 0) return;   /* 请求已被拒绝（413），不再接收数据 */
    if (c->rlen >= sizeof(c->rbuf) - 1) {   /* 读缓冲满仍未收齐：拒绝 */
        if (c->hdr_done)
            http_send_response(c->fd, 413, "Payload Too Large", "text/plain", "request too large", 17);
        c->done = 1;
        conn_drain(c);
        if (!c->closed) conn_update_events(c);
        return;
    }

    ssize_t r = read(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - 1 - c->rlen);
    if (r <= 0) {
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        conn_close(c);
        return;
    }
    c->rlen += (size_t)r;
    c->rbuf[c->rlen] = '\0';
    c->last_active = time(NULL);

    if (!c->hdr_done) {
        const char *sep = strstr(c->rbuf, "\r\n\r\n");
        if (!sep) sep = strstr(c->rbuf, "\n\n");
        if (sep) {
            c->hdr_done = 1;
            c->hdr_len = (size_t)(sep - c->rbuf) + (sep[0] == '\r' ? 4 : 2);
            sscanf(c->rbuf, "%15s %255s", c->method, c->uri);
            long cl = http_content_length(c->rbuf);
            if (cl > 0) c->body_cl = cl;
            /* body 上限：普通请求须收进读缓冲；文件上传落盘后不受读缓冲限制 */
            long max_body = conn_is_ai_upload(c) ? HTTP_UPLOAD_MAX : HTTP_BODY_MAX;
            if (c->body_cl > max_body) {   /* body 过大：拒绝，避免读缓冲溢出 */
                http_send_response(c->fd, 413, "Payload Too Large", "text/plain", "body too large", 15);
                c->body_cl = -1;
                c->done = 1;
                conn_drain(c);
                if (!c->closed) conn_update_events(c);
                return;
            }
            if (conn_is_ai_upload(c) && c->body_cl > 0) {
                snprintf(c->up_path, sizeof(c->up_path), "/tmp/ai_upload_%d.tmp", c->fd);
                c->up_fp = fopen(c->up_path, "wb");
                c->up_cl = c->body_cl;
                if (!c->up_fp) {
                    LOG_ERROR("http: cannot create upload temp file %s", c->up_path);
                    c->up_path[0] = 0;
                    c->up_cl = 0;
                }
            }
        }
    }

    if (c->hdr_done) {
        size_t body_got = (c->rlen > c->hdr_len) ? c->rlen - c->hdr_len : 0;
        if (c->up_fp) {   /* 上传连接：body 写入临时文件，读缓冲压缩回头部 */
            if (body_got > 0) {
                if (fwrite(c->rbuf + c->hdr_len, 1, body_got, c->up_fp) != body_got) {
                    conn_close(c);
                    return;
                }
                c->up_got += (long)body_got;
                c->rlen = c->hdr_len;
                c->rbuf[c->hdr_len] = '\0';
            }
            if (c->up_got >= c->up_cl) {
                fclose(c->up_fp);
                c->up_fp = NULL;
                c->done = 1;
                http_process_request(c);
            }
            return;
        }
        if (body_got >= (size_t)c->body_cl) {
            c->done = 1;
            http_process_request(c);
        }
    }
}

/* 空闲超时清理：长时间无 IO 进展的连接直接断开 */
static void conn_sweep(time_t now)
{
    http_conn_t *c = g_conns;
    while (c) {
        http_conn_t *nxt = c->next;
        if (now - c->last_active > HTTP_CONN_IDLE) conn_close(c);
        c = nxt;
    }
}

/* ---- HTTP 主循环 ---- */

void *http_server_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    g_http_app = app;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { LOG_ERROR("http socket"); return NULL; }

    if (set_nonblock(g_listen_fd) < 0) {
        LOG_ERROR("http set nonblocking");
        pthread_mutex_lock(&g_listen_mutex);
        close(g_listen_fd);
        g_listen_fd = -1;
        pthread_mutex_unlock(&g_listen_mutex);
        return NULL;
    }

    /* 允许端口在 TIME_WAIT 残留时立即复用，避免重启后 bind 失败 */
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(HTTP_DEFAULT_PORT);

    /* bind/listen 失败不退出：记录原因并定期重试（持续喂狗），保证 Web 服务最终恢复 */
    while (app->running) {
        if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
            listen(g_listen_fd, 10) == 0)
            break;
        LOG_ERROR("http bind :%d failed: %s, retrying in 2s...",
                  HTTP_DEFAULT_PORT, strerror(errno));
        watchdog_feed_self("http");
        usleep(2 * 1000 * 1000);
    }
    if (!app->running) {
        pthread_mutex_lock(&g_listen_mutex);
        close(g_listen_fd);
        g_listen_fd = -1;
        pthread_mutex_unlock(&g_listen_mutex);
        return NULL;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        LOG_ERROR("http epoll_create1");
        pthread_mutex_lock(&g_listen_mutex);
        close(g_listen_fd);
        g_listen_fd = -1;
        pthread_mutex_unlock(&g_listen_mutex);
        return NULL;
    }
    g_epfd = epfd;

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
        LOG_ERROR("http epoll_ctl add listen");
        close(epfd);
        g_epfd = -1;
        pthread_mutex_lock(&g_listen_mutex);
        close(g_listen_fd);
        g_listen_fd = -1;
        pthread_mutex_unlock(&g_listen_mutex);
        return NULL;
    }

    LOG_INFO("HTTP server listening on port %d", HTTP_DEFAULT_PORT);

    struct epoll_event events[64];
    while (app->running) {
        int n = epoll_wait_feed(epfd, events, sizeof(events) / sizeof(events[0]), 500, "http");
        if (n < 0) break;

        time_t now = time(NULL);

        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            if (fd == g_listen_fd) {
                while (1) {
                    struct sockaddr_in client;
                    socklen_t len = sizeof(client);
                    int client_fd = accept(g_listen_fd, (struct sockaddr *)&client, &len);
                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                        if (errno == EINTR) continue;
                        LOG_ERROR("http accept");
                        break;
                    }
                    if (__atomic_fetch_add(&g_http_active, 1, __ATOMIC_RELAXED) >= HTTP_MAX_CONN) {
                        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
                        LOG_ERROR("http: too many connections, rejecting %s",
                                  inet_ntoa(client.sin_addr));
                        close(client_fd);
                        continue;
                    }
                    set_nonblock(client_fd);   /* 客户端 socket 全部非阻塞 */
                    http_conn_t *c = conn_new(client_fd);
                    if (!c) {
                        close(client_fd);
                        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
                        continue;
                    }
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                        LOG_ERROR("http epoll_ctl add client");
                        conn_close(c);
                    }
                }
                continue;
            }

            http_conn_t *c = conn_find(fd);
            if (!c) continue;

            uint32_t evm = events[i].events;
            if (evm & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                conn_close(c);
                continue;
            }
            if (evm & EPOLLIN) conn_read(c);
            if (c->closed) continue;
            if (evm & EPOLLOUT) conn_drain(c);
            if (c->closed) continue;
            conn_update_events(c);
        }

        /* 空闲超时清理 */
        conn_sweep(now);
        /* 释放已关闭连接的残留结构体（必须在本轮事件处理全部结束后） */
        conn_reap();
    }

    /* 关闭所有残留连接 */
    while (g_conns) conn_close(g_conns);
    conn_reap();
    close(epfd);
    g_epfd = -1;
    /* 所有 g_listen_fd 关闭点都加锁：http_server_stop（watchdog 强杀路径）
       与主线程退出路径可能并发，互斥避免 double-close */
    pthread_mutex_lock(&g_listen_mutex);
    close(g_listen_fd);
    g_listen_fd = -1;
    pthread_mutex_unlock(&g_listen_mutex);
    LOG_INFO("HTTP server stopped");
    return NULL;
}

/* ---- 公共接口 ---- */

int http_server_start(void *arg)
{
    (void)arg;
    /* HTTP 端口固定为 80（见 http.h 的 HTTP_DEFAULT_PORT） */
    LOG_INFO("HTTP port = %d", HTTP_DEFAULT_PORT);
    return 0;
}

void http_server_stop(void *arg)
{
    (void)arg;
    /* 加锁关闭：与 http_server_task 退出路径并发时避免 double-close；
       shutdown() 用于唤醒阻塞中的 epoll_wait */
    pthread_mutex_lock(&g_listen_mutex);
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    pthread_mutex_unlock(&g_listen_mutex);
}
