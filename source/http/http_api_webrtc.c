/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * http_api_webrtc.c — /api/webrtc/whep 反向代理：把浏览器 WHEP SDP 握手
 * 转发到本机 mediamtx（HTTP 8889，/live/whep），应答回传浏览器。
 *
 * 仅转发 SDP 信令（几 KB）；WebRTC 媒体（DTLS/SRTP/RTP）走浏览器 ↔
 * mediamtx 直连 UDP，不经过本服务。同源代理免去跨域问题。
 *
 * 阻塞 IO：后端在本机回环，往返毫秒级；Reactor 单线程可接受。
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "http/http_internal.h"
#include "http/http_util.h"
#include "core/log.h"

#define MTX_HOST      "127.0.0.1"
#define MTX_PORT      8889
#define MTX_PATH      "/live/whep"
#define MTX_CONN_TIMEOUT_MS 2000
#define MTX_READ_TIMEOUT_MS 5000
#define MTX_ANS_MAX   (16 * 1024)

/* 连接后端（非阻塞 connect + poll 超时）；失败返回 -1 */
static int mtx_connect(void)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(MTX_PORT);
    if (inet_pton(AF_INET, MTX_HOST, &sa.sin_addr) != 1) { close(fd); return -1; }
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc < 0 && errno == EINPROGRESS) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT };
        rc = poll(&pfd, 1, MTX_CONN_TIMEOUT_MS);
        if (rc <= 0) { close(fd); return -1; }
        int soerr = 0; socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
            close(fd); return -1;
        }
    } else if (rc < 0) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fl);   /* 恢复阻塞：本地回环往返极小，直接阻塞读写 */
    return fd;
}

static int mtx_write_all(int fd, const char *d, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, d + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* 转发 SDP offer 到 mediamtx，读回 answer（malloc，含 HTTP 头，
   头尾间偏移由调用方用 strstr 定位；整体限长 16KB）。
   返回 0 成功；-1 失败 */
static int mtx_whep_exchange(const char *offer, size_t offer_len,
                             char **out, size_t *out_len)
{
    *out = NULL; *out_len = 0;
    int fd = mtx_connect();
    if (fd < 0) return -1;

    char hdr[512];
    int hl = snprintf(hdr, sizeof(hdr),
                      "POST %s HTTP/1.1\r\n"
                      "Host: %s:%d\r\n"
                      "Content-Type: application/sdp\r\n"
                      "Content-Length: %zu\r\n"
                      "Accept: application/sdp\r\n"
                      "Connection: close\r\n\r\n",
                      MTX_PATH, MTX_HOST, MTX_PORT, offer_len);
    if (mtx_write_all(fd, hdr, (size_t)hl) != 0 ||
        mtx_write_all(fd, offer, offer_len) != 0) { close(fd); return -1; }

    char *buf = malloc(MTX_ANS_MAX);
    if (!buf) { close(fd); return -1; }
    size_t total = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, MTX_READ_TIMEOUT_MS);
        if (pr <= 0) { free(buf); close(fd); return -1; }
        ssize_t n = read(fd, buf + total, MTX_ANS_MAX - total - 1);
        if (n <= 0) break;
        total += (size_t)n;
        if (total >= MTX_ANS_MAX - 1) break;
    }
    close(fd);
    if (total == 0) { free(buf); return -1; }
    buf[total] = '\0';
    *out = buf;
    *out_len = total;
    return 0;
}

/* SDP 行迭代：返回下一行起始（含 \r\n），无则 NULL；@n 为该行长度 */
static const char *sdp_next_line(const char *p, const char *end, size_t *n)
{
    if (p >= end) return NULL;
    const char *eol = strstr(p, "\r\n");
    size_t len = (eol && eol < end) ? (size_t)(eol - p) + 2 : (size_t)(end - p);
    *n = len;
    return p;
}

/* 浏览器 offer 缺 H265 时的兼容改写：把最后一个 H264 负载重写为 H265
   （负载号不变，删除其 fmtp 行）。Chrome 的平台解码器支持 HEVC，但
   WebRTC 协商门控未在 offer 提供 H265；复用既有负载号后，mediamtx 会
   以 H265 应答，Chrome 按 answer 的 rtpmap 建立 H265 解码器。
   返回 malloc 的新 offer（*out_len），无需改写返回 NULL */
static char *sdp_force_h265(const char *body, size_t blen, size_t *out_len)
{
    if (blen == 0 || strstr(body, "H265")) return NULL;
    const char *end = body + blen;

    /* 找最后一个 a=rtpmap:N H264/90000 行 */
    const char *line = NULL, *codec = NULL;
    size_t llen = 0;
    int pt = 0;
    const char *p = body;
    size_t n;
    while ((p = sdp_next_line(p, end, &n))) {
        if (n >= 13 && strncmp(p, "a=rtpmap:", 9) == 0) {
            const char *sp = strchr(p, ' ');
            if (sp && (size_t)(sp - p) < n && strncmp(sp + 1, "H264/90000", 10) == 0) {
                line = p; llen = n; pt = atoi(p + 9); codec = sp + 1;
            }
        }
        p += n;
    }
    if (!line) return NULL;

    /* 找对应 a=fmtp:<pt> 行（改写后删除，避免 H264 特有参数干扰解析） */
    char fmtp[32];
    snprintf(fmtp, sizeof(fmtp), "a=fmtp:%d ", pt);
    const char *fline = NULL;
    size_t flen = 0;
    p = body;
    while ((p = sdp_next_line(p, end, &n))) {
        if (n >= strlen(fmtp) && strncmp(p, fmtp, strlen(fmtp)) == 0) {
            fline = p; flen = n;
        }
        p += n;
    }

    size_t newlen = blen - (fline ? flen : 0);
    char *nb = malloc(newlen + 1);
    if (!nb) return NULL;
    if (fline) {
        /* fmtp 行整体跳过（rtpmap 行在其前，前缀偏移不受影响） */
        size_t foff = (size_t)(fline - body);
        memcpy(nb, body, foff);
        memcpy(nb + foff, body + foff + flen, blen - foff - flen);
    } else {
        memcpy(nb, body, blen);
    }
    size_t coff = (size_t)(codec - body);
    nb[coff] = 'H'; nb[coff + 1] = '2'; nb[coff + 2] = '6'; nb[coff + 3] = '5';
    nb[newlen] = '\0';
    *out_len = newlen;
    return nb;
}

void http_webrtc_whep(app_ctx_t *app, int fd, const char *method,
                      const char *uri, const char *req_buf)
{
    (void)app; (void)uri;
    if (strcmp(method, "POST") != 0) {
        http_err(fd, 405, "Method Not Allowed", NULL);
        return;
    }
    const char *body = http_body_start(req_buf);
    long cl = http_content_length(req_buf);
    size_t blen = (cl >= 0) ? (size_t)cl : strlen(body);
    if (blen == 0 || blen > MTX_ANS_MAX) {
        http_err(fd, 400, "Bad Request", "missing SDP offer\n");
        return;
    }

    /* H265 兼容注入：offer 无 H265 → 重写一个 H264 负载为 H265 */
    char *mod = NULL;
    size_t mlen = 0;
    if ((mod = sdp_force_h265(body, blen, &mlen)) != NULL) {
        LOG_INFO("webrtc: offer lacks H265 — rewrote payload to H265");
        body = mod;
        blen = mlen;
    }

    char *ans = NULL; size_t alen = 0;
    if (mtx_whep_exchange(body, blen, &ans, &alen) != 0) {
        if (mod) free(mod);
        LOG_ERROR("webrtc: mediamtx WHEP exchange failed (backend unreachable?)");
        http_err(fd, 503, "Service Unavailable", "webrtc unavailable\n");
        return;
    }
    if (mod) free(mod);
    /* mediamtx WHEP 成功应答为 201 Created（资源已建） */
    if (strncmp(ans, "HTTP/1.1 201", 12) != 0 &&
        strncmp(ans, "HTTP/1.1 200", 12) != 0 &&
        strncmp(ans, "HTTP/1.0 200", 12) != 0) {
        free(ans);
        LOG_ERROR("webrtc: mediamtx WHEP rejected offer");
        http_err(fd, 503, "Service Unavailable", "webrtc rejected\n");
        return;
    }
    const char *sdp = strstr(ans, "\r\n\r\n");
    if (!sdp) {
        free(ans);
        http_err(fd, 502, "Bad Gateway", "empty answer\n");
        return;
    }
    sdp += 4;
    http_send_response(fd, 200, "OK", "application/sdp", sdp, alen - (size_t)(sdp - ans));
    free(ans);
}
