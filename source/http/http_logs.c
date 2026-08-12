#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
/**
 * http_logs.c — 日志管理路由（列表 / 下载 / 删除 / 打包）。
 */

#include "http/http_internal.h"
#include "watchdog/watchdog.h"

#define LOG_DIR PATH_LOGS
#define LOG_PACK_MAX (100UL * 1024 * 1024)  /* 日志打包体积上限 100MB，防止长时间阻塞 HTTP */

/* 日志文件列表页面 */
static void serve_log_list(int fd)
{
    DIR *dir = opendir(LOG_DIR);
    if (!dir) { http_handle_404(fd, "/logs"); return; }

    char *html = NULL;
    size_t cap = 4096, len = 0;
    html = malloc(cap);
    if (!html) {
        closedir(dir);
        http_send_response(fd, 500, "Error", "text/plain", "", 0);
        return;
    }

    len += snprintf(html + len, cap - len,
        "<!DOCTYPE html><html lang=\"zh\"><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>日志文件</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}"
        "body{font-family:Segoe UI,sans-serif;background:linear-gradient(135deg,#0b1021,#0f1b33,#0b1628);color:#c8d6e5;padding:24px;min-height:100vh}"
        "h1{color:#78c0f0;font-size:22px;text-shadow:0 0 12px rgba(108,180,238,.2)}"
        "a{color:#78c0f0;text-decoration:none}"
        "a:hover{text-decoration:underline}"
        ".top-bar{display:flex;align-items:center;gap:12px;margin-bottom:16px;flex-wrap:wrap}"
        ".top-bar h1{margin:0}"
        ".btn{padding:8px 14px;border-radius:8px;border:none;cursor:pointer;font-size:13px;text-decoration:none;display:inline-block;backdrop-filter:blur(8px)}"
        ".btn-pack{background:rgba(30,111,212,.7);color:#fff}"
        ".tbl-wrap{overflow-x:auto;-webkit-overflow-scrolling:touch;margin-top:8px;border-radius:10px}"
        "table{border-collapse:collapse;width:100%%;min-width:480px;background:rgba(16,29,51,.5);backdrop-filter:blur(12px);border:1px solid rgba(108,180,238,.1);border-radius:10px;overflow:hidden}"
        "th,td{padding:8px 12px;text-align:left;border-bottom:1px solid rgba(108,180,238,.08);white-space:nowrap}"
        "th{color:#8b9ec4;font-size:12px}"
        "td{font-size:13px}"
        ".del{background:rgba(192,57,78,.8);color:#fff;border:none;padding:3px 8px;border-radius:4px;cursor:pointer;font-size:11px;margin-left:8px}"
        ".dl{color:#78c0f0}"
        ".back-link{display:inline-block;padding:6px 12px;background:rgba(255,255,255,.05);backdrop-filter:blur(8px);border:1px solid rgba(108,180,238,.1);border-radius:8px;font-size:13px;margin-bottom:12px}"
        "@media(max-width:540px){"
        "body{padding:12px}"
        "h1{font-size:18px}"
        "th,td{padding:6px 8px;font-size:11px}"
        ".btn{padding:6px 10px;font-size:12px}"
        ".del{padding:2px 6px;font-size:10px;margin-left:4px}"
        "}"
        "</style></head><body>"
        "<a href=\"/\" class=\"back-link\">← 返回首页</a>"
        "<div class=\"top-bar\"><h1>📋 日志文件</h1>"
        "<a href=\"/logs/pack\" class=\"btn btn-pack\">📦 打包下载</a></div>"
        "<div class=\"tbl-wrap\"><table><tr><th>文件名</th><th>大小</th><th>修改时间</th><th>操作</th></tr>");

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char path[512];
        snprintf(path, sizeof(path), "%s/%s", LOG_DIR, de->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;
        if (!S_ISREG(st.st_mode)) continue;

        char time_str[32];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M",
                 localtime(&st.st_mtime));

        double ksize = st.st_size / 1024.0;
        const char *unit = "KB";
        if (ksize > 1024) { ksize /= 1024; unit = "MB"; }

        int need = snprintf(NULL, 0,
            "<tr><td>%s</td><td>%.1f %s</td><td>%s</td>"
            "<td><a class=\"dl\" href=\"/logfile/%s\">下载</a> "
            "<button class=\"del\" onclick=\"fetch('/logfile/%s',{method:'DELETE'}).then(()=>location.reload())\">删除</button></td></tr>",
            de->d_name, ksize, unit, time_str, de->d_name, de->d_name);
        if (len + need + 128 > cap) {
            cap = len + need + 128;
            char *tmp = realloc(html, cap);
            if (!tmp) { free(html); closedir(dir); http_send_response(fd, 500, "Error", "text/plain", "", 0); return; }
            html = tmp;
        }
        len += snprintf(html + len, cap - len,
            "<tr><td>%s</td><td>%.1f %s</td><td>%s</td>"
            "<td><a class=\"dl\" href=\"/logfile/%s\">下载</a> "
            "<button class=\"del\" onclick=\"fetch('/logfile/%s',{method:'DELETE'}).then(()=>location.reload())\">删除</button></td></tr>",
            de->d_name, ksize, unit, time_str, de->d_name, de->d_name);
    }
    closedir(dir);

    len += snprintf(html + len, cap - len, "</table></div></body></html>");
    http_send_response(fd, 200, "OK", "text/html; charset=utf-8", html, len);
    free(html);
}

/* 下载单个日志文件 */
static void serve_log_download(int fd, const char *filename)
{
    if (strstr(filename, "..") || strchr(filename, '/')) {
        http_handle_404(fd, filename);
        return;
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, filename);

    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, filename); return; }

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
        filename, size);
    write(fd, header, off);

    char buf[4096];
    size_t remain = size;
    while (remain > 0) {
        size_t n = (remain > sizeof(buf)) ? sizeof(buf) : remain;
        size_t r = fread(buf, 1, n, fp);
        if (r == 0) break;
        write(fd, buf, r);
        remain -= r;
    }
    fclose(fp);
}

/* 删除日志文件 */
static void serve_log_delete(int fd, const char *filename)
{
    if (strstr(filename, "..") || strchr(filename, '/')) {
        http_send_response(fd, 400, "Bad Request", "text/plain", "", 0);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LOG_DIR, filename);
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
    write(fd, header, strlen(header));

    char buf[8192];
    size_t n, total = 0;
    while ((n = fread(buf, 1, sizeof(buf), tar)) > 0) {
        write(fd, buf, n);
        total += n;
        if (total > LOG_PACK_MAX) break;   /* 限制打包体积，避免长时间阻塞 HTTP 服务器 */
        watchdog_feed(WD_HTTP);            /* 打包耗时较长，持续喂狗防止看门狗误杀 */
    }
    pclose(tar);
}

/* 日志总入口 */
void http_logs_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)req_buf;
    if (strcmp(uri, "/logs") == 0)
        serve_log_list(fd);
    else if (strcmp(uri, "/logs/pack") == 0)
        serve_log_pack(fd);
    else if (strncmp(uri, "/logfile/", 9) == 0) {
        if (strcmp(method, "DELETE") == 0)
            serve_log_delete(fd, uri + 9);
        else
            serve_log_download(fd, uri + 9);
    }
}
