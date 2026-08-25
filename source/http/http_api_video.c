/**
 * http_api_video.c — V4L2 设备列表 & 格式/分辨率查询
 */

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include "http/http_internal.h"

/* ---- /api/video/devices —— 枚举可用的摄像头 ---- */
void http_video_devices(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)uri; (void)req_buf;
    char json[2048];
    int off = 0, first = 1;
    JSON_ADD(json, off, "[");

    for (int i = 0; i < 10; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/video%d", i);
        int dev_fd = open(path, O_RDONLY | O_NONBLOCK);
        if (dev_fd < 0) continue;

        struct v4l2_capability cap;
        if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) == 0 &&
            (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            char card[64];
            int ci = 0;
            for (int j = 0; cap.card[j] && ci < 60; j++) {
                char c = cap.card[j];
                if (c == '"' || c == '\\') { card[ci++] = '\\'; card[ci++] = c; }
                else if (c >= 32 && c < 127) card[ci++] = c;
            }
            card[ci] = '\0';
            JSON_ADD(json, off, "%s{\"path\":\"%s\",\"card\":\"%s\"}",
                     first ? "" : ",", path, card);
            first = 0;
        }
        close(dev_fd);
    }
    JSON_ADD(json, off, "]");
    http_send_response(fd, 200, "OK", "application/json", json, off);
}

/* ---- /api/video/caps —— 返回预设格式与分辨率（避免二次 open 驱动卡死）---- */
void http_video_caps(app_ctx_t *app, int fd, const char *method, const char *uri, const char *req_buf)
{
    (void)app; (void)method; (void)req_buf;

    const char *q = strchr(uri, '?');
    const char *device = "/dev/video0";
    if (q) {
        const char *d = strstr(q + 1, "device=");
        if (d) {
            const char *val = d + 7;
            const char *p = val;
            while (*p && *p != '&') p++;
            size_t len = (size_t)(p - val);
            /* 仅允许 /dev/videoN（N 为十进制数字），防止 open 任意路径 */
            if (len > 10 && strncmp(val, "/dev/video", 10) == 0) {
                int valid = 1;
                for (size_t k = 10; k < len; k++)
                    if (val[k] < '0' || val[k] > '9') { valid = 0; break; }
                if (valid) device = val;
            }
        }
    }

    /* 尝试快速 QUERYCAP 获取设备名，失败则用占位名 */
    char card[64] = "Unknown Camera";
    int dev_fd = open(device, O_RDONLY | O_NONBLOCK);
    if (dev_fd >= 0) {
        struct v4l2_capability cap;
        if (ioctl(dev_fd, VIDIOC_QUERYCAP, &cap) == 0) {
            int ci = 0;
            for (int j = 0; cap.card[j] && ci < 60; j++) {
                char c = cap.card[j];
                if (c == '"' || c == '\\') { card[ci++] = '\\'; card[ci++] = c; }
                else if (c >= 32 && c < 127) card[ci++] = c;
            }
            card[ci] = '\0';
        }
        close(dev_fd);
    }

    /* 预设常用分辨率（不再调用 ENUM_FRAMESIZES，避免驱动卡死） */
    static const struct { int w, h; const char *label; } presets[] = {
        { 320,  240, "QVGA" },
        { 640,  480, "VGA" },
        { 800,  600, "SVGA" },
        { 1024, 768, "XGA" },
        { 1280, 720, "HD" },
        { 1920,1080, "FHD" },
        { 2560,1440, "2K" },
        { 3840,2160, "4K" },
    };
#define NP (int)(sizeof(presets)/sizeof(presets[0]))

    char json[4096];
    int off = 0;
    JSON_ADD(json, off,
        "{\"device\":\"%s\",\"card\":\"%s\",\"formats\":["
        "{\"fmt\":\"MJPG\",\"desc\":\"MJPEG\",\"sizes\":[", device, card);

    for (int i = 0; i < NP; i++)
        JSON_ADD(json, off, "%s{\"w\":%d,\"h\":%d,\"label\":\"%s\"}",
                 i > 0 ? "," : "", presets[i].w, presets[i].h, presets[i].label);

    JSON_ADD(json, off, "]},"
        "{\"fmt\":\"YUYV\",\"desc\":\"YUYV 4:2:2\",\"sizes\":[");

    for (int i = 0; i < NP; i++)
        JSON_ADD(json, off, "%s{\"w\":%d,\"h\":%d,\"label\":\"%s\"}",
                 i > 0 ? "," : "", presets[i].w, presets[i].h, presets[i].label);

    JSON_ADD(json, off, "]}]}");
    http_send_response(fd, 200, "OK", "application/json", json, off);
#undef NP
}
