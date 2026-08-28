/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_rec.c — 网络录像控制路由：
 *   POST /api/rec/start   开始录制（AI 画框帧优先）
 *   POST /api/rec/stop    停止录制并 finalize
 *   GET  /api/rec/status  录制状态
 *   GET  /api/rec/list    录制文件列表
 *   GET  /api/rec/pack    打包下载全部录像 (tar.gz)
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
#define REC_PACK_MAX (2UL * 1024 * 1024 * 1024)  /* 录像打包体积上限 2GB，防止长时间阻塞 HTTP / 占满磁盘 */

/* 单文件名合法性：rec_*.mp4 */
static int rec_file_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 9 || strncmp(name, "rec_", 4) != 0 ||
        strcmp(name + len - 4, ".mp4") != 0)
        return 0;
    for (size_t i = 4; i + 4 < len; i++)
        if (name[i] != '-' && name[i] != '_' &&
            (name[i] < '0' || name[i] > '9'))
            return 0;
    return 1;
}

/* 解析录制相对路径：要求 YYYYMMDD/rec_*.mp4 形式且不含 '..'（防路径穿越） */
static int rec_resolve_rel(const char *rel, char *subdir, size_t subdir_size,
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
    if (!rec_file_valid(name)) return -1;
    return 0;
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

/* 录制文件列表（按 mtime 倒序；两层：YYYYMMDD/rec_*.mp4） */
static void serve_rec_list(int fd)
{
    struct ent { char name[160]; time_t mtime; int64_t size; };
    static struct ent ents[1024];
    int n = 0;
    int cap = (int)(sizeof(ents)/sizeof(ents[0]));

    DIR *dir = opendir(REC_DIR);
    if (!dir) {
        http_err(fd, 500, "Error", NULL);
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL && n < cap) {
        if (de->d_name[0] == '.') continue;
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", REC_DIR, de->d_name);
        struct stat dst;
        if (stat(subdir, &dst) < 0 || !S_ISDIR(dst.st_mode)) continue;

        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *fde;
        while ((fde = readdir(sd)) != NULL && n < cap) {
            if (fde->d_name[0] == '.') continue;
            if (!rec_file_valid(fde->d_name)) continue;
            char path[512];
            snprintf(path, sizeof(path), "%s/%s", subdir, fde->d_name);
            struct stat st;
            if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
            snprintf(ents[n].name, sizeof(ents[n].name), "%s/%s",
                     de->d_name, fde->d_name);
            ents[n].mtime = st.st_mtime;
            ents[n].size  = (int64_t)st.st_size;
            n++;
        }
        closedir(sd);
    }
    closedir(dir);

    /* mtime 倒序：新录制在前 */
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (ents[j].mtime > ents[i].mtime) {
                struct ent t = ents[i]; ents[i] = ents[j]; ents[j] = t;
            }

    char json[65536];
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

/* 下载：/recfile/<YYYYMMDD/rec_*.mp4> */
static void serve_rec_download(int fd, const char *rel)
{
    char subdir[64], name[256];
    if (rec_resolve_rel(rel, subdir, sizeof(subdir), name, sizeof(name)) != 0) {
        http_handle_404(fd, rel);
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", REC_DIR, subdir, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) { http_handle_404(fd, rel); return; }
    size_t size = http_file_size(fp);
    char disp[320];
    snprintf(disp, sizeof(disp),
             "Content-Disposition: attachment; filename=\"%s\"\r\n", name);
    http_serve_stream(fd, "video/mp4", disp, fp, size, NULL);
}

/* 删除：body = YYYYMMDD/rec_*.mp4 */
static void serve_rec_delete(int fd, const char *req)
{
    char name[160];
    /* req 是完整 HTTP 请求，必须提取 body（此前直接当 body 用 → 永远 400） */
    const char *body = http_body_start(req);
    if (!body || !*body) { http_err(fd, 400, "Bad Request", NULL); return; }
    safe_strncpy(name, sizeof(name), body);
    /* 去掉尾部空白/换行 */
    size_t len = strlen(name);
    while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r' ||
                       name[len-1] == ' ')) name[--len] = '\0';
    char subdir[64], fname[256];
    if (rec_resolve_rel(name, subdir, sizeof(subdir), fname, sizeof(fname)) != 0) {
        http_err(fd, 400, "Bad Request", NULL);
        return;
    }
    /* 拒绝删除正在录制的文件：unlink 后录制句柄仍在写，数据丢失且磁盘不释放 */
    video_rec_status_t st;
    video_rec_status(&st);
    if (st.recording && st.file[0] && strcmp(st.file, name) == 0) {
        http_err(fd, 409, "Conflict", "recording in progress");
        return;
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s/%s", REC_DIR, subdir, fname);
    if (unlink(path) == 0)
        http_ok_text(fd, "ok");
    else
        http_err(fd, 404, "Not Found", NULL);
}

/* 一键清空录像：删除全部录像文件，保留正在录制的文件 */
static void serve_rec_clear(int fd)
{
    video_rec_status_t st;
    video_rec_status(&st);
    const char *active = (st.recording && st.file[0]) ? st.file : NULL;

    int del = 0;
    DIR *dir = opendir(REC_DIR);
    if (!dir) { http_err(fd, 500, "Error", NULL); return; }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", REC_DIR, de->d_name);
        struct stat dst;
        if (stat(subdir, &dst) < 0 || !S_ISDIR(dst.st_mode)) continue;
        DIR *sd = opendir(subdir);
        if (!sd) continue;
        struct dirent *fde;
        while ((fde = readdir(sd)) != NULL) {
            if (fde->d_name[0] == '.') continue;
            if (!rec_file_valid(fde->d_name)) continue;   /* 跳过打包临时文件等 */
            char rel[160];
            snprintf(rel, sizeof(rel), "%s/%s", de->d_name, fde->d_name);
            if (active && strcmp(rel, active) == 0) continue;   /* 保留录制中的文件 */
            char path[512];
            snprintf(path, sizeof(path), "%s/%s/%s", REC_DIR, de->d_name, fde->d_name);
            if (unlink(path) == 0) del++;
        }
        closedir(sd);
    }
    closedir(dir);
    char json[64];
    int off = snprintf(json, sizeof(json), "{\"deleted\":%d}", del);
    http_ok_json(fd, json, (size_t)off);
}

/* 统计录像目录总大小（含按天子目录），用于打包前的体积限制 */
static int64_t rec_total_size(void)
{
    int64_t total = 0;
    DIR *dir = opendir(REC_DIR);
    if (!dir) return 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subdir[512];
        snprintf(subdir, sizeof(subdir), "%s/%s", REC_DIR, de->d_name);
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
            if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) total += (int64_t)st.st_size;
        }
        closedir(sd);
    }
    closedir(dir);
    return total;
}

/* 打包下载所有录像 (tar.gz)：临时文件放 recordings/ 内（保证 systemd ProtectSystem
   下可写），打包时用 --exclude 排除自身；完成后流式发送并自动清理 */
static void serve_rec_pack(int fd)
{
    if (rec_total_size() > (int64_t)REC_PACK_MAX) {
        LOG_INFO("rec pack: total size exceeds %lld bytes, rejected", (long long)REC_PACK_MAX);
        http_err(fd, 413, "Payload Too Large", "recordings too large");
        return;
    }

    char tmppath[512];
    snprintf(tmppath, sizeof(tmppath), "%s/.recs_pack_%d.tar.gz", REC_DIR, (int)getpid());
    char cmd[1400];
    snprintf(cmd, sizeof(cmd),
             "tar -czf '%s' -C '%s' --exclude='.recs_pack_*' . 2>/dev/null",
             tmppath, REC_DIR);
    if (system(cmd) != 0 || access(tmppath, F_OK) != 0) {
        unlink(tmppath);
        http_err(fd, 500, "Internal Error", NULL);
        return;
    }

    FILE *fp = fopen(tmppath, "rb");
    if (!fp) {
        unlink(tmppath);
        http_err(fd, 500, "Internal Error", NULL);
        return;
    }
    size_t size = http_file_size(fp);

    http_serve_stream(fd, "application/gzip",
                      "Content-Disposition: attachment; filename=\"recordings_pack.tar.gz\"\r\n",
                      fp, size, tmppath);
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
    } else if (strcmp(uri, "/api/rec/pack") == 0) {
        serve_rec_pack(fd);
    } else if (strcmp(uri, "/api/rec/delete") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        serve_rec_delete(fd, req_buf);
    } else if (strcmp(uri, "/api/rec/clear") == 0) {
        if (strcmp(method, "POST") != 0) { http_err(fd, 405, "Method Not Allowed", NULL); return; }
        serve_rec_clear(fd);
    } else if (strncmp(uri, "/recfile/", 9) == 0) {
        char name[128];
        http_url_decode(uri + 9, name, sizeof(name));
        serve_rec_download(fd, name);
    } else {
        http_handle_404(fd, uri);
    }
}
