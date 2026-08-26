/**
 * http_api_rec.c — 网络录像控制路由：
 *   POST /api/rec/start   开始录制（AI 画框帧优先）
 *   POST /api/rec/stop    停止录制并 finalize
 *   GET  /api/rec/status  录制状态
 *   GET  /api/rec/list    录制文件列表
 *   POST /api/rec/delete  删除录制文件（body = 文件名）
 *   GET  /recfile/<name>  下载录制文件
 */

#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include "http/http_internal.h"
#include "video/video_rec.h"

#define REC_DIR PATH_RECORDINGS

/* 录制文件名合法性：rec_*.mp4 且不含路径分隔/..（防路径穿越） */
static int rec_name_valid(const char *name)
{
    size_t len;
    if (!name || !*name) return 0;
    if (strstr(name, "..") || strchr(name, '/') || strchr(name, '\\')) return 0;
    len = strlen(name);
    if (len < 9 || strncmp(name, "rec_", 4) != 0 ||
        strcmp(name + len - 4, ".mp4") != 0)
        return 0;
    for (size_t i = 4; i + 4 < len; i++)
        if (name[i] != '-' && name[i] != '_' &&
            (name[i] < '0' || name[i] > '9'))
            return 0;
    return 1;
}

/* 状态 JSON */
static void serve_rec_status(int fd)
{
    video_rec_status_t st;
    char json[512];
    int off;
    video_rec_status(&st);
    off = snprintf(json, sizeof(json),
        "{\"recording\":%d,\"file\":\"%s\",\"start_ms\":%llu,"
        "\"frames\":%llu,\"bytes\":%llu,\"fps\":%.1f}",
        st.recording, st.file[0] ? st.file : "",
        (unsigned long long)st.start_ms,
        (unsigned long long)st.frames, (unsigned long long)st.bytes, st.fps);
    http_ok_json(fd, json, (size_t)off);
}

/* 录制文件列表（按 mtime 倒序） */
static void serve_rec_list(int fd)
{
    struct ent { char name[128]; time_t mtime; int64_t size; };
    static struct ent ents[256];
    int n = 0;

    DIR *dir = opendir(REC_DIR);
    if (!dir) {
        http_err(fd, 500, "Error", NULL);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < (int)(sizeof(ents)/sizeof(ents[0]))) {
        if (de->d_name[0] == '.') continue;
        if (!rec_name_valid(de->d_name)) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", REC_DIR, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        safe_strncpy(ents[n].name, sizeof(ents[n].name), de->d_name);
        ents[n].mtime = st.st_mtime;
        ents[n].size  = (int64_t)st.st_size;
        n++;
    }
    closedir(dir);

    /* mtime 倒序：新录制在前 */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (ents[j].mtime > ents[i].mtime) {
                struct ent t = ents[i]; ents[i] = ents[j]; ents[j] = t;
            }

    char json[16384];
    int off = snprintf(json, sizeof(json), "{\"files\":[");
    for (int i = 0; i < n; i++) {
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&ents[i].mtime));
        off = http_json_append(json, sizeof(json), off,
            "%s{\"name\":\"%s\",\"size\":%lld,\"mtime\":\"%s\"}",
            i ? "," : "", ents[i].name, (long long)ents[i].size, ts);
        if (off < 0) break;
    }
    off = http_json_append(json, sizeof(json), off, "]}");
    http_ok_json(fd, json, (size_t)off);
}

/* 下载：/recfile/<name> */
static void serve_rec_download(int fd, const char *name)
{
    if (!rec_name_valid(name)) { http_handle_404(fd, name); return; }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", REC_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, name); return; }
    size_t size = http_file_size(fp);
    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, "video/mp4", disp, fp, size, NULL);
}

/* 删除：body = 文件名 */
static void serve_rec_delete(int fd, const char *body)
{
    char name[128];
    if (!body || !*body) { http_err(fd, 400, "Bad Request", NULL); return; }
    safe_strncpy(name, sizeof(name), body);
    /* 去掉尾部空白/换行 */
    size_t len = strlen(name);
    while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r' ||
                       name[len-1] == ' ')) name[--len] = '\0';
    if (!rec_name_valid(name)) { http_err(fd, 400, "Bad Request", NULL); return; }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s", REC_DIR, name);
    if (unlink(path) == 0)
        http_ok_text(fd, "ok");
    else
        http_err(fd, 404, "Not Found", NULL);
}

/* 总入口 */
void http_rec_handler(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app;
    if (strcmp(uri, "/api/rec/status") == 0) {
        serve_rec_status(fd);
    } else if (strcmp(uri, "/api/rec/list") == 0) {
        serve_rec_list(fd);
    } else if (strcmp(uri, "/api/rec/start") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        if (video_rec_start() != 0)
            http_err(fd, 409, "Conflict", "already recording");
        else
            http_ok_text(fd, "started");
    } else if (strcmp(uri, "/api/rec/stop") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        if (video_rec_stop() != 0)
            http_err(fd, 409, "Conflict", "not recording");
        else
            http_ok_text(fd, "stopped");
    } else if (strcmp(uri, "/api/rec/delete") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        serve_rec_delete(fd, req_buf);
    } else if (strncmp(uri, "/recfile/", 9) == 0) {
        char name[128];
        http_url_decode(uri + 9, name, sizeof(name));
        serve_rec_download(fd, name);
    } else {
        http_handle_404(fd, uri);
    }
}
