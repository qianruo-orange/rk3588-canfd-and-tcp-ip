/**
 * http.c — HTTP 核心：主循环、路由分发、认证、静态文件服务、工具函数。
 */

#define _GNU_SOURCE   /* 暴露 GNU/Linux 扩展 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <time.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>
#include <shadow.h>
#include <crypt.h>
#include <pwd.h>

#include "http/http_internal.h"
#include "video/video_stream.h"
#include "watchdog/watchdog.h"
#include "core/common.h"

/* ---- 全局变量 ---- */
static int g_listen_fd = -1;
static _Atomic int g_http_active = 0;   /* 活跃 HTTP 连接数 */
#define HTTP_MAX_CONN 64

typedef void (*api_fn)(app_ctx_t *, int, const char *, const char *, const char *);

/**
 * http_system_api_wrap - 包装 /api/system 处理函数，忽略不需要的 HTTP 参数并转发到实际实现。
 */
static void http_system_api_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_system_api(app, fd); }

/**
 * http_can_status_wrap - 包装 CAN 状态查询接口，统一调用底层 API。
 */
static void http_can_status_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_can_status(app, fd); }

/**
 * http_can_toggle_wrap - 包装 CAN 开关接口，保留请求体参数用于切换逻辑。
 */
static void http_can_toggle_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; http_can_toggle(app, fd, req); }

/**
 * http_can_decoded_wrap - 包装 DBC 解码结果查询接口。
 */
static void http_can_decoded_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_can_decoded(app, fd); }

/**
 * http_can_decoded_tx_wrap - 包装发送方向 DBC 解析结果查询接口。
 */
static void http_can_decoded_tx_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_can_decoded_tx(app, fd); }

/**
 * http_can_dbc_upload_wrap - 包装 DBC 文件上传接口，保留 method/uri/body 供上传处理。
 */
static void http_can_dbc_upload_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { http_can_dbc_upload(app, fd, method, uri, req); }

/**
 * http_can_send_wrap - 包装 CAN 报文发送接口，保留 body 供解析。
 */
static void http_can_send_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { http_can_send(app, fd, method, uri, req); }

/**
 * http_can_rx_wrap - 包装 CAN 原始报文查询接口。
 */
static void http_can_rx_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_can_rx(app, fd); }

/**
 * http_reboot_wrap - 包装重启接口，执行系统级重启动作。
 */
static void http_reboot_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_reboot(app, fd); }

/**
 * http_shutdown_wrap - 包装关机接口，执行系统级关机动作。
 */
static void http_shutdown_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_shutdown(app, fd); }

/**
 * http_network_api_wrap - 包装网络统计 API，统一走通用网络处理逻辑。
 */
static void http_network_api_wrap(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req) { (void)method; (void)uri; (void)req; http_network_api(app, fd); }

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

/**
 * http_write_all - 完整写入非阻塞 socket（处理 EAGAIN/EWOULDBLOCK 与部分写入）。
 * @fd: 客户端 socket。
 * @data: 数据指针。
 * @len: 数据长度。
 * @return: 成功返回 0；对端关闭或等待可写超时返回 -1。
 */
int http_write_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd = { .fd = fd, .events = POLLOUT };
                if (poll(&pfd, 1, 3000) <= 0) return -1;   /* 等待可写超时视为失败 */
                continue;
            }
            return -1;
        }
        return -1;   /* write 返回 0，视为对端异常 */
    }
    return 0;
}

/**
 * http_send_response - 向客户端发送标准 HTTP 响应头和响应体。
 * @fd: 客户端 socket。
 * @code: HTTP 状态码。
 * @status: 状态描述文本。
 * @mime: Content-Type。
 * @body: 响应体指针。
 * @len: 响应体长度。
 */
void http_send_response(int fd, int code, const char *status,
                        const char *mime, const void *body, size_t len)
{
    char header[512];
    int off = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       code, status, mime, len);
    if (off < 0) return;
    if (http_write_all(fd, header, (size_t)off) < 0) return;
    if (body && len > 0)
        http_write_all(fd, body, len);
}

/**
 * http_handle_404 - 处理未找到的静态资源请求，并返回 404 页面。
 * @fd: 客户端 socket。
 * @path: 请求路径。
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

/* internal helper: check basic auth; if require_root==1 require uid==0 */
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

    const char *auth = strstr(req, "Authorization: Basic ");
    if (!auth) goto deny;

    char decoded[128] = {0};
    const char *enc = auth + 21;
    int di = 0;
    for (int i = 0; enc[i] && enc[i] != '\r' && enc[i] != '\n' && di < 120; i++) {
        if (enc[i] >= 'A' && enc[i] <= 'Z') decoded[di] = (char)(enc[i] - 'A');
        else if (enc[i] >= 'a' && enc[i] <= 'z') decoded[di] = (char)(enc[i] - 'a' + 26);
        else if (enc[i] >= '0' && enc[i] <= '9') decoded[di] = (char)(enc[i] - '0' + 52);
        else if (enc[i] == '+') decoded[di] = 62;
        else if (enc[i] == '/') decoded[di] = 63;
        else continue;
        ++di;
    }

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
    if (ok && require_root && pw->pw_uid != 0) ok = 0;
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
    dprintf(fd,
        "HTTP/1.1 401 Unauthorized\r\n"
        "WWW-Authenticate: Basic realm=\"data_transport\"\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n");
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
 * http_serve_file - 根据 URI 提供静态文件服务，支持 HTML/CSS/JS 等前端资源。
 * @fd: 客户端 socket。
 * @uri: 请求路径。
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

    fseek(fp, 0, SEEK_END);
    size_t size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size > 20 * 1024 * 1024) {   /* 静态文件上限 20MB，防止大文件长时间阻塞 HTTP */
        fclose(fp);
        http_send_response(fd, 413, "Payload Too Large", "text/plain", "file too large", 15);
        return;
    }

    char header[256];
    int off = snprintf(header, sizeof(header),
                       "HTTP/1.1 200 OK\r\n"
                       "Content-Type: %s\r\n"
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n\r\n",
                       http_mime_type(path), size);
    if (http_write_all(fd, header, (size_t)off) < 0) { fclose(fp); return; }

    char buf[8192];
    size_t remain = size;
    while (remain > 0) {
        size_t n = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        size_t r = fread(buf, 1, n, fp);
        if (r == 0) break;
        if (http_write_all(fd, buf, r) < 0) break;
        remain -= r;
    }
    fclose(fp);
}

/* ---- 每连接处理线程 ---- */

/**
 * http_wait_fd - 用 epoll 实现单 fd 带超时的事件等待（可读/可写），替代 poll。
 * @fd: 目标描述符。
 * @events: 关注的事件（EPOLLIN / EPOLLOUT）。
 * @timeout_ms: 超时毫秒数。
 * @return: 就绪事件数（1 表示就绪，0 表示超时，-1 表示错误）。
 */
static int http_wait_fd(int fd, uint32_t events, int timeout_ms)
{
    int epfd = epoll_create1(0);
    if (epfd < 0) return -1;
    struct epoll_event ev = { .events = events, .data.fd = fd };
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) < 0) { close(epfd); return -1; }
    struct epoll_event out;
    int r = epoll_wait(epfd, &out, 1, timeout_ms);
    close(epfd);
    return r;
}

/**
 * client_handler - 处理单个 HTTP 客户端连接，解析请求并分发到对应 API 或静态文件逻辑。
 * @app: 应用上下文。
 * @fd: 客户端连接描述符。
 * @return: 线程退出时返回 NULL。
 */
static void *client_handler(app_ctx_t *app, int fd)
{
    (void)app;
    char buf[HTTP_BUF_SIZE];
    const long max_body_bytes = 1024L * 1024L; /* 1MB */

    ssize_t n;
    /* 等待首个请求包到达（最多 30 秒）：fd 是非阻塞的，若数据未到立即 read
       会返回 EAGAIN 而被当作连接异常关闭，慢客户端/网络拥塞时易误断 */
    if (http_wait_fd(fd, EPOLLIN, 30000) <= 0) {
        close(fd);
        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(fd);
        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    buf[n] = '\0';

    /* 持续读取直到收到完整请求（\r\n\r\n 或超时） */
    int tries = 0;
    while (!strstr(buf, "\r\n\r\n") && !strstr(buf, "\n\n") && tries < 5) {
        if (http_wait_fd(fd, EPOLLIN, 100) <= 0) break;
        ssize_t r = read(fd, buf + n, sizeof(buf) - 1 - n);
        if (r <= 0) break;
        n += r; if (n >= (ssize_t)sizeof(buf)-1) n = (ssize_t)sizeof(buf)-1; buf[n] = '\0'; tries++;
    }

    /* 读取 POST body —— 最多等 1 秒 */
    const char *cl_hdr = strstr(buf, "Content-Length:");
    if (!cl_hdr) cl_hdr = strstr(buf, "content-length:");
    long cl = 0;
    if (cl_hdr) {
        char *end = NULL;
        cl = strtol(cl_hdr + 15, &end, 10);
        if (end == cl_hdr + 15 || cl < 0) cl = 0;
    }
    if (cl > max_body_bytes) {
        http_send_response(fd, 413, "Payload Too Large", "text/plain", "body too large", 15);
        close(fd);
        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
        return NULL;
    }
    const char *sep = strstr(buf, "\r\n\r\n");
    if (!sep) sep = strstr(buf, "\n\n");
    int hdr_len = sep ? (int)(sep - buf) + (sep[0] == '\r' ? 4 : 2) : 0;
    int body_read = (n > hdr_len && hdr_len > 0) ? (int)(n - hdr_len) : 0;
    for (int t = 0; body_read < cl && t < 10; t++) {
        if (http_wait_fd(fd, EPOLLIN, 100) <= 0) break;
        size_t avail = sizeof(buf) - 1 - (size_t)n;
        if (avail == 0) break;
        ssize_t r = read(fd, buf + n, avail);
        if (r <= 0) break;
        n += r; body_read += (int)r; if (n >= (ssize_t)sizeof(buf)-1) n = (ssize_t)sizeof(buf)-1; buf[n] = '\0';
    }
    /* body 未读完（超出缓冲区）——拒绝处理，避免配置被静默截断 */
    if (cl > 0 && body_read < cl) {
        http_send_response(fd, 413, "Payload Too Large", "text/plain", "body too large", 15);
        goto close;
    }

    char method[16] = "GET", uri[HTTP_URI_MAX] = "/";
    sscanf(buf, "%15s %255s", method, uri);

    /* ---- 路由分发 ---- */

    /* 视频 MJPEG：特殊处理（推流线程由 video 模块创建，每连接一个） */
    if (strcmp(uri, "/video/mjpeg") == 0) {
        if (video_stream_client_start(fd, http_video_client_closed) != 0) {
            http_send_response(fd, 500, "Error", "text/plain", "", 0);
            goto close;
        }
        return NULL;
    }

    /* 认证页面（文件服务 + auth）—— 认证涉及 crypt()，可能阻塞，先喂狗 */
    watchdog_feed_self("http");
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        if (!http_check_auth_user(buf, fd)) goto close;
        http_serve_file(fd, "/index.html"); goto close;
    }
    if (strcmp(uri, "/config") == 0 || strncmp(uri, "/config.html", 11) == 0) {
        if (!http_check_auth_root(buf, fd)) goto close;
        http_serve_file(fd, "/config.html"); goto close;
    }
    if (strcmp(uri, "/dbc") == 0 || strncmp(uri, "/dbc.html", 9) == 0) {
        if (!http_check_auth_user(buf, fd)) goto close;
        http_serve_file(fd, "/dbc.html"); goto close;
    }

    static const struct { const char *uri; int pre; const char *method; api_fn fn; int (*auth)(const char*,int); }
    rt[] = {
        { "/api/logs",      0, NULL,   http_logs_handler, http_check_auth_root },
        { "/logs",          1, NULL,   http_logs_handler, http_check_auth_root },
        { "/logfile/",      1, NULL,   http_logs_handler, http_check_auth_root },
        { "/api/system",    0, NULL,   http_system_api_wrap,   NULL },
        { "/api/can",       0, NULL,   http_can_status_wrap,   NULL },
        { "/api/can/decoded",0,NULL,   http_can_decoded_wrap,  NULL },
        { "/api/can/decoded/tx",0,NULL,http_can_decoded_tx_wrap, NULL },
        { "/api/can/dbc",    0, "POST", http_can_dbc_upload_wrap, http_check_auth_root },
        { "/api/can/send",   0, "POST", http_can_send_wrap,   http_check_auth_root },
        { "/api/can/frames", 0, NULL,   http_can_rx_wrap,     NULL },
        { "/api/can/toggle",0, "POST", http_can_toggle_wrap, http_check_auth_root },
        { "/api/config",    0, "POST", http_config_post,  http_check_auth_root },
        { "/api/config",    0, NULL,   http_config_get,   http_check_auth_root },
        { "/api/reboot",    0, NULL,   http_reboot_wrap,       http_check_auth_root },
        { "/api/shutdown",  0, NULL,   http_shutdown_wrap,     http_check_auth_root },
        { "/api/network",   0, NULL,   http_network_api_wrap,  NULL },
        { "/api/video/caps",1, NULL,  http_video_caps,   NULL },
        { "/api/video/devices",0,NULL, http_video_devices,NULL },
    };

    for (int i = 0; i < (int)(sizeof(rt)/sizeof(rt[0])); i++) {
        int m = rt[i].pre ? !strncmp(uri, rt[i].uri, strlen(rt[i].uri))
                          : !strcmp(uri, rt[i].uri);
        if (!m) continue;
        if (rt[i].method && strcmp(method, rt[i].method) != 0) continue;
        if (rt[i].auth && !rt[i].auth(buf, fd)) goto close;
        rt[i].fn(app, fd, method, uri, buf);
        goto close;
    }

    /* 兜底：静态文件（公开） */
    http_serve_file(fd, uri);

close:
    close(fd);
    __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
    return NULL;
}

/* 视频推流线程退出回调：关闭 fd 并归还连接计数 */
static void http_video_client_closed(int fd)
{
    close(fd);
    __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
}

/* ---- HTTP 主循环 ---- */

static int set_socket_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void *http_server_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { LOG_ERROR("http socket"); return NULL; }

    if (set_socket_nonblocking(g_listen_fd) < 0) {
        LOG_ERROR("http set nonblocking");
        close(g_listen_fd);
        g_listen_fd = -1;
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(HTTP_DEFAULT_PORT);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("http bind :%d", HTTP_DEFAULT_PORT);
        close(g_listen_fd); g_listen_fd = -1; return NULL;
    }
    if (listen(g_listen_fd, 10) < 0) {
        LOG_ERROR("http listen :%d", HTTP_DEFAULT_PORT);
        close(g_listen_fd); g_listen_fd = -1; return NULL;
    }

    int epfd = epoll_create1(0);
    if (epfd < 0) {
        LOG_ERROR("http epoll_create1");
        close(g_listen_fd);
        g_listen_fd = -1;
        return NULL;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_listen_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, g_listen_fd, &ev) < 0) {
        LOG_ERROR("http epoll_ctl add listen");
        close(epfd);
        close(g_listen_fd);
        g_listen_fd = -1;
        return NULL;
    }

    LOG_INFO("HTTP server listening on port %d", HTTP_DEFAULT_PORT);

    struct epoll_event events[64];
    while (app->running) {
        int n = epoll_wait(epfd, events, sizeof(events) / sizeof(events[0]), 500);
        if (n < 0) {
            if (errno == EINTR) { watchdog_feed_self("http"); continue; }
            break;
        }

        watchdog_feed_self("http");

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
                    /* 置非阻塞，避免慢客户端阻塞服务端 write */
                    set_socket_nonblocking(client_fd);
                    struct epoll_event cev;
                    cev.events = EPOLLIN | EPOLLRDHUP | EPOLLHUP;
                    cev.data.fd = client_fd;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev) < 0) {
                        LOG_ERROR("http epoll_ctl add client");
                        close(client_fd);
                        __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
                    }
                }
                continue;
            }

            if (events[i].events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR)) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                __atomic_fetch_sub(&g_http_active, 1, __ATOMIC_RELAXED);
                continue;
            }

            epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
            client_handler(app, fd);
        }
    }

    close(epfd);
    close(g_listen_fd);
    g_listen_fd = -1;
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
    if (g_listen_fd >= 0) {
        shutdown(g_listen_fd, SHUT_RDWR);
        close(g_listen_fd);
        g_listen_fd = -1;
    }
}
