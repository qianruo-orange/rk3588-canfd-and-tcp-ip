#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <ctype.h>
/**
 * http_logs.c — 日志管理路由（列表 / 下载 / 删除 / 打包）。
 */

#include "http/http_internal.h"
#include "watchdog/watchdog.h"

#define LOG_DIR PATH_LOGS
#define LOG_PACK_MAX (100UL * 1024 * 1024)  /* 日志打包体积上限 100MB，防止长时间阻塞 HTTP */

/* 用 epoll 实现单 fd 带超时的事件等待（可读/可写），替代 poll */
static int logs_wait_fd(int fd, uint32_t events, int timeout_ms)
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

/* URL 解码：前端用 encodeURIComponent 会把 '/' 编码为 %2F，这里还原 */
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, o = 0;
    while (src[i] && o + 1 < dst_size) {
        if (src[i] == '%' &&
            isxdigit((unsigned char)src[i+1]) &&
            isxdigit((unsigned char)src[i+2])) {
            int hi = isdigit((unsigned char)src[i+1]) ? src[i+1] - '0'
                                                      : (tolower((unsigned char)src[i+1]) - 'a' + 10);
            int lo = isdigit((unsigned char)src[i+2]) ? src[i+2] - '0'
                                                      : (tolower((unsigned char)src[i+2]) - 'a' + 10);
            dst[o++] = (char)((hi << 4) | lo);
            i += 3;
        } else {
            dst[o++] = src[i++];
        }
    }
    dst[o] = '\0';
}

/* 解析日志相对路径，要求为 YYYYMMDD/filename 形式且不含 '..' */
static int logs_resolve_rel(const char *rel, char *subdir, size_t subdir_size,
                            char *name, size_t name_size)
{
    if (!rel || !*rel || strstr(rel, "..")) return -1;
    const char *slash = strchr(rel, '/');
    if (!slash || strchr(slash + 1, '/')) return -1;  /* 只允许一个 '/' */
    size_t dlen = (size_t)(slash - rel);
    if (dlen == 0 || dlen >= subdir_size) return -1;
    memcpy(subdir, rel, dlen);
    subdir[dlen] = '\0';
    safe_strncpy(name, name_size, slash + 1);
    if (!*name) return -1;
    return 0;
}

/* 将数据完整写入非阻塞 socket（处理 EAGAIN/EWOULDBLOCK 与部分写入） */
static int http_write_all(int fd, const void *data, size_t len)
{
    const char *p = (const char *)data;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, p + off, len - off);
        if (w > 0) { off += (size_t)w; continue; }
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (logs_wait_fd(fd, EPOLLOUT, 1000) <= 0) return -1;
                continue;
            }
            if (errno == EINTR) continue;
            return -1;
        }
        return -1;   /* write 返回 0，视为错误 */
    }
    return 0;
}

/* 日志文件列表 JSON API */
static void serve_log_list_json(int fd)
{
    DIR *dir = opendir(LOG_DIR);
    if (!dir) {
        http_send_response(fd, 500, "Error", "application/json", "{\"logs\":[]}", 11);
        return;
    }

    char json[32768];
    int off = snprintf(json, sizeof(json), "{\"logs\":[");

    struct dirent *de;
    int first = 1;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", LOG_DIR, de->d_name);
        struct stat dst;
        if (stat(subdir, &dst) < 0 || !S_ISDIR(dst.st_mode)) continue;

        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *fde;
        while ((fde = readdir(sd)) != NULL) {
            if (fde->d_name[0] == '.') continue;

            char path[512];
            snprintf(path, sizeof(path), "%s/%s", subdir, fde->d_name);
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) continue;

            char time_str[32];
            strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                     localtime(&st.st_mtime));

            int n = snprintf(json + off, sizeof(json) - off,
                "%s{\"name\":\"%s/%s\",\"size\":%lld,\"mtime\":\"%s\"}",
                first ? "" : ",", de->d_name, fde->d_name,
                (long long)st.st_size, time_str);
            if (n < 0 || n >= (int)(sizeof(json) - off)) break;
            off += n;
            first = 0;
        }
        closedir(sd);
    }
    closedir(dir);

    off += snprintf(json + off, sizeof(json) - off, "]}");
    http_send_response(fd, 200, "OK", "application/json; charset=utf-8", json, (size_t)off);
}

/* 下载单个日志文件 */
static void serve_log_download(int fd, const char *rel)
{
    char subdir[64], name[256];
    if (logs_resolve_rel(rel, subdir, sizeof(subdir), name, sizeof(name)) != 0) {
        http_handle_404(fd, rel);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", LOG_DIR, subdir, name);

    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, rel); return; }

    fseek(fp, 0, SEEK_END);
    size_t size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char header[512];
    int off = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        name, size);
    if (http_write_all(fd, header, (size_t)off) < 0) { fclose(fp); return; }

    char buf[4096];
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

/* 删除日志文件 */
static void serve_log_delete(int fd, const char *rel)
{
    char subdir[64], name[256];
    if (logs_resolve_rel(rel, subdir, sizeof(subdir), name, sizeof(name)) != 0) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "", 0);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", LOG_DIR, subdir, name);
    if (unlink(path) == 0)
        http_send_response(fd, 200, "OK", "text/plain", "ok", 2);
    else
        http_send_response(fd, 404, "Not Found", "text/plain", "", 0);
}

/* 打包下载所有日志 (tar.gz) */
static void serve_log_pack(int fd)
{
    /* 直接在日志目录上执行 tar，避免 chdir 造成进程工作目录副作用 */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "tar -czf - -C %s . 2>/dev/null", LOG_DIR);
    FILE *tar = popen(cmd, "r");
    if (!tar) {
        log_error("logs pack: popen tar failed");
        http_send_response(fd, 500, "Internal Error", "text/plain", "", 0);
        return;
    }

    char header[256];
    snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/gzip\r\n"
        "Content-Disposition: attachment; filename=\"logs_pack.tar.gz\"\r\n"
        "Connection: close\r\n\r\n");
    if (http_write_all(fd, header, strlen(header)) < 0) { pclose(tar); return; }

    char buf[8192];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), tar)) > 0) {
        if (http_write_all(fd, buf, n) < 0) break;
        total += n;
        if (total > LOG_PACK_MAX) break;   /* 限制打包体积，避免长时间阻塞 HTTP 服务器 */
        watchdog_feed_self("http");            /* 打包耗时较长，持续喂狗防止看门狗误杀 */
    }
    pclose(tar);
}

/* 日志总入口 */
void http_logs_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)req_buf;
    if (strcmp(uri, "/logs") == 0 || strcmp(uri, "/logs.html") == 0)
        http_serve_file(fd, "/logs.html");
    else if (strcmp(uri, "/api/logs") == 0)
        serve_log_list_json(fd);
    else if (strcmp(uri, "/logs/pack") == 0)
        serve_log_pack(fd);
    else if (strncmp(uri, "/logfile/", 9) == 0) {
        char rel[512];
        url_decode(uri + 9, rel, sizeof(rel));
        if (strcmp(method, "DELETE") == 0)
            serve_log_delete(fd, rel);
        else
            serve_log_download(fd, rel);
    }
}
