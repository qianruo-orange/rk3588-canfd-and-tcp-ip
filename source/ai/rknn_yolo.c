/**
 * rknn_yolo.c — RKNN + YOLO26 检测框架。
 *
 * 数据流（由 rknn_ai_task 线程驱动）：
 *   video_stream_get_frame（取最新采集帧 JPEG/YUYV）
 *   → 解码/转换 RGB24 → 双线性缩放到模型输入
 *   → rknn_inputs_set / rknn_run / rknn_outputs_get
 *   → YOLO26 后处理（端到端无 NMS 单头为主，兼容经典三头 + NMS）
 *   → 检测结果快照 + 画框编码 JPEG 快照（供 /video/mjpeg_ai 推流）
 *
 * 优雅降级：无模型 / NPU 驱动未加载 / 推理失败 → enabled=0，原视频流照常，
 * 画框流客户端回退到原始帧。所有失败路径只记日志不崩溃。
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <math.h>
#include <jpeglib.h>

#include "rknn/rknn_api.h"

#include "ai/rknn_yolo.h"
#include "core/common.h"
#include "core/log.h"
#include "core/config.h"
#include "watchdog/watchdog.h"
#include "video/video_stream.h"

#define AI_MAX_OUTPUTS 4
#define AI_JPEG_QUALITY 85

/* 与 ai/rknn_yolo.h 的解耦：画框帧快照类型在此定义（须在 g_ai 之前） */
typedef struct {
    unsigned char    *data;
    size_t            len;
    unsigned long long seq;
    int               w, h;
} yolo_frame_t;

/* 模块全局上下文：所有访问都在 ai 线程 / init / destroy 内，单写者；
   检测结果与画框帧快照各自用互斥锁保护（供推流线程并发读） */
static struct {
    int          enabled;         /* 模型加载成功且可推理 */
    int          running;
    rknn_context ctx;
    char         model_path[256];

    int          in_w, in_h;      /* 模型输入尺寸（来自模型属性） */
    uint32_t     n_output;
    rknn_tensor_attr out_attr[AI_MAX_OUTPUTS];
    int          nc;              /* 类别数（按输出布局推断） */
    float        conf, nms;
    int          interval_ms;

    unsigned char *in_buf;        /* 缩放后的 RGB 输入缓冲 */

    unsigned long long last_seq;  /* 最近处理过的视频帧序号（去重） */

    pthread_mutex_t result_mutex;
    yolo_result_t   result;

    pthread_mutex_t frame_mutex;
    yolo_frame_t   *frame;        /* 最新画框 JPEG 快照 */

    app_ctx_t *app;
} g_ai;

static int ai_logged_layout = 0;   /* 输出布局不支持时只警告一次 */

/* ---- JPEG 解码（libjpeg-turbo）---- */
static int jpeg_to_rgb(const unsigned char *jpeg, size_t jpeg_len,
                       unsigned char **rgb_out, int *w_out, int *h_out)
{
    struct jpeg_decompress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)jpeg, jpeg_len);
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);
    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    unsigned char *rgb = malloc((size_t)w * h * 3);
    if (!rgb) {
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    unsigned char *row = malloc((size_t)w * 3);
    if (!row) {
        free(rgb);
        jpeg_destroy_decompress(&cinfo);
        return -1;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) break;
        memcpy(rgb + (size_t)cinfo.output_scanline * w * 3, row, (size_t)w * 3);
    }
    free(row);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    *rgb_out = rgb;
    *w_out = w;
    *h_out = h;
    return 0;
}

/* ---- JPEG 编码（libjpeg-turbo）---- */
static int rgb_to_jpeg(const unsigned char *rgb, int w, int h,
                       unsigned char **out, size_t *out_len)
{
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    jpeg_mem_dest(&cinfo, &mem, &memlen);

    cinfo.image_width      = (JDIMENSION)w;
    cinfo.image_height     = (JDIMENSION)h;
    cinfo.input_components = 3;
    cinfo.in_color_space   = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, AI_JPEG_QUALITY, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    unsigned char *row = malloc((size_t)w * 3);
    if (!row) {
        jpeg_destroy_compress(&cinfo);
        if (mem) free(mem);
        return -1;
    }
    while (cinfo.next_scanline < cinfo.image_height) {
        memcpy(row, rgb + (size_t)cinfo.next_scanline * w * 3, (size_t)w * 3);
        jpeg_write_scanlines(&cinfo, &row, 1);
    }
    free(row);
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    *out = mem;
    *out_len = (size_t)memlen;
    return 0;
}

/* ---- YUYV → RGB24（BT.601）---- */
static void yuyv_to_rgb(const unsigned char *src, int w, int h, unsigned char *dst)
{
    int npix = w * h / 2;
    for (int i = 0; i < npix; i++) {
        int y0 = src[0], u = src[1] - 128, y1 = src[2], v = src[3] - 128;
        src += 4;
        for (int k = 0; k < 2; k++) {
            int y = k ? y1 : y0;
            int r = (int)(y + 1.402f * v);
            int g = (int)(y - 0.344f * u - 0.714f * v);
            int b = (int)(y + 1.772f * u);
            if (r < 0) r = 0; else if (r > 255) r = 255;
            if (g < 0) g = 0; else if (g > 255) g = 255;
            if (b < 0) b = 0; else if (b > 255) b = 255;
            *dst++ = (unsigned char)r;
            *dst++ = (unsigned char)g;
            *dst++ = (unsigned char)b;
        }
    }
}

/* ---- RGB24 双线性缩放（全图拉伸，非 letterbox）---- */
static void rgb_resize(const unsigned char *src, int sw, int sh,
                       unsigned char *dst, int dw, int dh)
{
    for (int y = 0; y < dh; y++) {
        float sy = (y + 0.5f) * sh / dh - 0.5f;
        if (sy < 0) sy = 0;
        int y0 = (int)sy;
        int y1 = y0 + 1 < sh ? y0 + 1 : y0;
        float fy = sy - y0;
        for (int x = 0; x < dw; x++) {
            float sx = (x + 0.5f) * sw / dw - 0.5f;
            if (sx < 0) sx = 0;
            int x0 = (int)sx;
            int x1 = x0 + 1 < sw ? x0 + 1 : x0;
            float fx = sx - x0;
            const unsigned char *p00 = src + ((size_t)y0 * sw + x0) * 3;
            const unsigned char *p01 = src + ((size_t)y0 * sw + x1) * 3;
            const unsigned char *p10 = src + ((size_t)y1 * sw + x0) * 3;
            const unsigned char *p11 = src + ((size_t)y1 * sw + x1) * 3;
            for (int c = 0; c < 3; c++) {
                float v = p00[c] * (1 - fx) * (1 - fy) + p01[c] * fx * (1 - fy) +
                          p10[c] * (1 - fx) * fy + p11[c] * fx * fy;
                *dst++ = (unsigned char)(v + 0.5f);
            }
        }
    }
}

/* ---- 画框（RGB24，2px 边框，顶角色块标记类别）---- */
static void draw_box(unsigned char *rgb, int w, int h,
                     int x1, int y1, int x2, int y2, unsigned int color)
{
    int r = (int)((color >> 16) & 0xFF), g = (int)((color >> 8) & 0xFF), b = (int)(color & 0xFF);
    if (x1 < 0) x1 = 0; if (y1 < 0) y1 = 0;
    if (x2 >= w) x2 = w - 1; if (y2 >= h) y2 = h - 1;
    if (x2 <= x1 || y2 <= y1) return;

    for (int y = y1; y <= y2; y++) {
        for (int x = x1; x <= x2; x++) {
            int edge = (x - x1 < 2 || x2 - x < 2 || y - y1 < 2 || y2 - y < 2);
            if (!edge) continue;
            unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
        }
    }
    /* 左上角色块：无字体时用色块标记类别 */
    int tw = x2 - x1 < 16 ? x2 - x1 : 16;
    int th = y2 - y1 < 8  ? y2 - y1 : 8;
    for (int y = y1; y < y1 + th; y++)
        for (int x = x1; x < x1 + tw; x++) {
            unsigned char *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b;
        }
}

static float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

static float clamp01(float v)
{
    return v < 0 ? 0 : (v > 1 ? 1 : v);
}

/* ---- YOLO26 后处理 ----
   布局 1（端到端无 NMS，YOLO26 一对一检测头，单输出）：
     输出形状 [1, N, 4+nc] 或 [1, 4+nc, N] 或 [N, 4+nc]，行内为
     cx,cy,w,h（输入坐标系）+ nc 个类别分数（导出时通常已插入 sigmoid）。
   布局 2（经典三头，YOLOv8 风格，3 输出）：
     各输出 [1, H*W, 4+nc]（NHWC）或 [1, 4+nc, H, W]（NCHW），
     4+nc 为原始 logits，需 sigmoid 后按网格 stride 解码，再做 NMS。
   @return 检测数（已按 conf 过滤并缩放回原图坐标） */
static int yolo_postprocess(const float **out_buf, const rknn_tensor_attr *attrs,
                            uint32_t n_output, int frame_w, int frame_h,
                            yolo_det_t *dets, int max_dets, float conf, float nms)
{
    float sx = (float)frame_w / (float)g_ai.in_w;
    float sy = (float)frame_h / (float)g_ai.in_h;
    int ndet = 0;
    float cand[YOLO_MAX_DETS * 6];   /* x1,y1,x2,y2,score,cls */
    int ncand = 0;

    if (n_output == 1) {
        const rknn_tensor_attr *a = &attrs[0];
        uint32_t nd = a->n_dims;
        uint32_t ch = 0, N = 0;
        if (nd == 3 && a->dims[1] < a->dims[2]) { ch = a->dims[1]; N = a->dims[2]; }
        else if (nd == 3)                       { N = a->dims[1]; ch = a->dims[2]; }
        else if (nd == 2)                       { N = a->dims[0]; ch = a->dims[1]; }
        else { if (!ai_logged_layout) { ai_logged_layout = 1; LOG_ERROR("ai: unsupported e2e output dims=%u", nd); } return 0; }
        if (ch < 5 || N == 0 || (uint64_t)N * ch != a->n_elems) {
            if (!ai_logged_layout) { ai_logged_layout = 1; LOG_ERROR("ai: e2e layout mismatch ch=%u N=%u elems=%u", ch, N, a->n_elems); }
            return 0;
        }
        int nc = (int)ch - 4;
        g_ai.nc = nc;
        const float *buf = out_buf[0];
        /* 探测是否需要 sigmoid：任意类别分 >1 说明是原始 logits */
        int need_sigmoid = 0;
        for (uint32_t i = 0; i < N && !need_sigmoid; i++) {
            const float *row = buf + (size_t)i * ch + 4;
            for (int c = 0; c < nc; c++)
                if (row[c] > 1.001f) { need_sigmoid = 1; break; }
        }
        for (uint32_t i = 0; i < N && ncand < (int)sizeof(cand)/6; i++) {
            const float *row = buf + (size_t)i * ch;
            int best = 0;
            float bests = need_sigmoid ? sigmoid(row[4]) : clamp01(row[4]);
            for (int c = 1; c < nc; c++) {
                float s = need_sigmoid ? sigmoid(row[4 + c]) : clamp01(row[4 + c]);
                if (s > bests) { bests = s; best = c; }
            }
            if (bests < conf) continue;
            float cx = row[0], cy = row[1], bw = row[2], bh = row[3];
            float x1 = (cx - bw * 0.5f) * sx, y1 = (cy - bh * 0.5f) * sy;
            float x2 = (cx + bw * 0.5f) * sx, y2 = (cy + bh * 0.5f) * sy;
            cand[ncand * 6 + 0] = x1; cand[ncand * 6 + 1] = y1;
            cand[ncand * 6 + 2] = x2; cand[ncand * 6 + 3] = y2;
            cand[ncand * 6 + 4] = bests; cand[ncand * 6 + 5] = (float)best;
            ncand++;
        }
    } else if (n_output == 3) {
        /* 经典三头：先收集全部候选（sigmoid + stride 解码），再 NMS */
        int nc = 0;
        for (uint32_t o = 0; o < n_output; o++)
            if (attrs[o].n_dims >= 3) {
                uint32_t ch = (attrs[o].fmt == RKNN_TENSOR_NHWC && attrs[o].n_dims == 3)
                              ? attrs[o].dims[2] : attrs[o].dims[1];
                if (ch > (uint32_t)nc) nc = (int)ch;
            }
        nc -= 4;
        g_ai.nc = nc > 0 ? nc : 80;
        if (nc <= 0) nc = 80;
        for (uint32_t o = 0; o < n_output && ncand < (int)sizeof(cand)/6; o++) {
            const rknn_tensor_attr *a = &attrs[o];
            const float *buf = out_buf[o];
            uint32_t nd = a->n_dims;
            uint32_t ch = 0, H = 0, W = 0;
            int nchw = 0;
            if (nd == 4) { nchw = 1; ch = a->dims[1]; H = a->dims[2]; W = a->dims[3]; }
            else if (nd == 3 && a->dims[1] < a->dims[2]) { nchw = 1; ch = a->dims[1]; H = a->dims[2]; W = a->dims[3] ? a->dims[3] : 1; }
            else if (nd == 3) { ch = a->dims[2]; W = a->dims[1]; H = 1; }
            else { if (!ai_logged_layout) { ai_logged_layout = 1; LOG_ERROR("ai: unsupported 3-head dims=%u", nd); } return 0; }
            if (ch < 5 || H == 0 || W == 0) continue;
            uint32_t A = H * W;
            if ((uint64_t)A * ch != a->n_elems) continue;
            int stride = (int)roundf((float)g_ai.in_w / (float)W);
            if (stride <= 0) stride = 8;
            for (uint32_t a_i = 0; a_i < A && ncand < (int)sizeof(cand)/6; a_i++) {
                /* NHWC：row[a_i*ch+c]；NCHW：row[c*A+a_i] */
                const float *row;
                float tmp[5];
                int cx0, cy0;
                if (nchw) {
                    /* 网格坐标 a_i = gy*W + gx */
                    cy0 = (int)(a_i / W); cx0 = (int)(a_i % W);
                    for (int c = 0; c < 5; c++) tmp[c] = buf[(size_t)c * A + a_i];
                    row = tmp;
                } else {
                    cy0 = (int)(a_i / W); cx0 = (int)(a_i % W);
                    row = buf + (size_t)a_i * ch;
                }
                float bs = sigmoid(row[4]);
                if (bs < conf) continue;
                int best = 0;
                float bests = bs;
                for (int c = 1; c < nc; c++) {
                    float s = sigmoid(nchw ? buf[(size_t)(4 + c) * A + a_i] : row[4 + c]);
                    if (s > bests) { bests = s; best = c; }
                }
                if (bests < conf) continue;
                float x = (sigmoid(row[0]) * 2.0f - 0.5f + (float)cx0) * (float)stride;
                float y = (sigmoid(row[1]) * 2.0f - 0.5f + (float)cy0) * (float)stride;
                float bw = sigmoid(row[2]) * sigmoid(row[2]) * 4.0f * (float)stride;
                float bh = sigmoid(row[3]) * sigmoid(row[3]) * 4.0f * (float)stride;
                cand[ncand * 6 + 0] = (x - bw * 0.5f) * sx; cand[ncand * 6 + 1] = (y - bh * 0.5f) * sy;
                cand[ncand * 6 + 2] = (x + bw * 0.5f) * sx; cand[ncand * 6 + 3] = (y + bh * 0.5f) * sy;
                cand[ncand * 6 + 4] = bests; cand[ncand * 6 + 5] = (float)best;
                ncand++;
            }
        }
        /* NMS（类别内抑制） */
        static char suppressed[YOLO_MAX_DETS];
        for (int i = 0; i < ncand; i++) suppressed[i] = 0;
        for (int i = 0; i < ncand && ndet < max_dets; i++) {
            if (suppressed[i]) continue;
            float bx1 = cand[i*6+0], by1 = cand[i*6+1], bx2 = cand[i*6+2], by2 = cand[i*6+3];
            float bi = (bx2 - bx1) * (by2 - by1);
            for (int j = i + 1; j < ncand; j++) {
                if (suppressed[j]) continue;
                if (cand[i*6+5] != cand[j*6+5]) continue;   /* 仅同类抑制 */
                float ix1 = bx1 > cand[j*6+0] ? bx1 : cand[j*6+0];
                float iy1 = by1 > cand[j*6+1] ? by1 : cand[j*6+1];
                float ix2 = bx2 < cand[j*6+2] ? bx2 : cand[j*6+2];
                float iy2 = by2 < cand[j*6+3] ? by2 : cand[j*6+3];
                if (ix2 <= ix1 || iy2 <= iy1) continue;
                float inter = (ix2 - ix1) * (iy2 - iy1);
                float bj = (cand[j*6+2] - cand[j*6+0]) * (cand[j*6+3] - cand[j*6+1]);
                float uni = bi + bj - inter;
                if (uni <= 0) continue;
                if (inter / uni > nms) suppressed[j] = 1;
            }
            dets[ndet].x1 = bx1; dets[ndet].y1 = by1;
            dets[ndet].x2 = bx2; dets[ndet].y2 = by2;
            dets[ndet].conf = cand[i*6+4]; dets[ndet].cls = (int)cand[i*6+5];
            ndet++;
        }
        return ndet;
    } else {
        if (!ai_logged_layout) { ai_logged_layout = 1; LOG_ERROR("ai: unsupported output count=%u (expect 1 or 3)", n_output); }
        return 0;
    }

    /* 端到端：直接输出（无 NMS） */
    for (int i = 0; i < ncand && ndet < max_dets; i++) {
        dets[ndet].x1 = cand[i*6+0]; dets[ndet].y1 = cand[i*6+1];
        dets[ndet].x2 = cand[i*6+2]; dets[ndet].y2 = cand[i*6+3];
        dets[ndet].conf = cand[i*6+4]; dets[ndet].cls = (int)cand[i*6+5];
        ndet++;
    }
    return ndet;
}

/* 模型文件读入内存；失败返回 -1 */
static int load_model_file(const char *path, unsigned char **buf_out, size_t *len_out)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return -1; }
    long sz = ftell(fp);
    if (sz <= 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return -1; }
    unsigned char *buf = malloc((size_t)sz);
    if (!buf) { fclose(fp); return -1; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return -1; }
    fclose(fp);
    *buf_out = buf;
    *len_out = (size_t)sz;
    return 0;
}

int rknn_yolo_init(void *arg)
{
    memset(&g_ai, 0, sizeof(g_ai));
    app_ctx_t *app = (app_ctx_t *)arg;
    g_ai.app = app;
    g_ai.ctx = 0;
    g_ai.conf = 0.25f;
    g_ai.nms = 0.45f;
    g_ai.interval_ms = 200;
    g_ai.running = 1;
    pthread_mutex_init(&g_ai.result_mutex, NULL);
    pthread_mutex_init(&g_ai.frame_mutex, NULL);

    if (!app || !app->cfg || !app->cfg->ai_enable) {
        LOG_INFO("ai: disabled by config");
        return 0;   /* enabled=0：task 直接退出 */
    }
    safe_strncpy(g_ai.model_path, sizeof(g_ai.model_path),
                 app->cfg->ai_model[0] ? app->cfg->ai_model : "config/yolo26.rknn");
    g_ai.conf = app->cfg->ai_conf > 0 ? app->cfg->ai_conf : 0.25f;
    g_ai.nms  = app->cfg->ai_nms  > 0 ? app->cfg->ai_nms  : 0.45f;
    g_ai.interval_ms = app->cfg->ai_interval_ms > 0 ? app->cfg->ai_interval_ms : 200;

    unsigned char *model = NULL;
    size_t mlen = 0;
    if (load_model_file(g_ai.model_path, &model, &mlen) < 0) {
        LOG_ERROR("ai: model '%s' not found, AI disabled (stream continues raw)",
                  g_ai.model_path);
        return 0;
    }
    int ret = rknn_init(&g_ai.ctx, model, (uint32_t)mlen, 0, NULL);
    free(model);
    if (ret != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_init failed (%d): model incompatible or NPU unavailable, AI disabled",
                  ret);
        g_ai.ctx = 0;
        return 0;
    }

    rknn_input_output_num io_num;
    if (rknn_query(g_ai.ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_query IN_OUT_NUM failed");
        goto fail_ctx;
    }
    if (io_num.n_input < 1 || io_num.n_output > AI_MAX_OUTPUTS) {
        LOG_ERROR("ai: unexpected io_num in=%u out=%u", io_num.n_input, io_num.n_output);
        goto fail_ctx;
    }
    g_ai.n_output = io_num.n_output;

    rknn_tensor_attr in_attr;
    memset(&in_attr, 0, sizeof(in_attr));
    in_attr.index = 0;
    if (rknn_query(g_ai.ctx, RKNN_QUERY_INPUT_ATTR, &in_attr, sizeof(in_attr)) != RKNN_SUCC) {
        LOG_ERROR("ai: rknn_query INPUT_ATTR failed");
        goto fail_ctx;
    }
    /* 输入尺寸：优先用模型静态尺寸；dims 全 0（动态模型）时用配置值 */
    if (in_attr.fmt == RKNN_TENSOR_NCHW && in_attr.n_dims >= 4) {
        g_ai.in_w = (int)in_attr.dims[3];
        g_ai.in_h = (int)in_attr.dims[2];
    } else if (in_attr.fmt == RKNN_TENSOR_NHWC && in_attr.n_dims >= 4) {
        g_ai.in_w = (int)in_attr.dims[2];
        g_ai.in_h = (int)in_attr.dims[1];
    }
    if (g_ai.in_w <= 0 || g_ai.in_h <= 0) {
        int s = app->cfg->ai_input_size > 0 ? app->cfg->ai_input_size : 640;
        g_ai.in_w = s;
        g_ai.in_h = s;
    }
    g_ai.in_buf = malloc((size_t)g_ai.in_w * g_ai.in_h * 3);
    if (!g_ai.in_buf) {
        LOG_ERROR("ai: input buffer alloc failed");
        goto fail_ctx;
    }

    for (uint32_t i = 0; i < g_ai.n_output; i++) {
        memset(&g_ai.out_attr[i], 0, sizeof(g_ai.out_attr[i]));
        g_ai.out_attr[i].index = i;
        if (rknn_query(g_ai.ctx, RKNN_QUERY_OUTPUT_ATTR, &g_ai.out_attr[i],
                       sizeof(g_ai.out_attr[i])) != RKNN_SUCC) {
            LOG_ERROR("ai: rknn_query OUTPUT_ATTR[%u] failed", i);
            goto fail_buf;
        }
    }

    g_ai.enabled = 1;
    LOG_INFO("ai: model '%s' loaded, input %dx%d, outputs %u, conf %.2f nms %.2f",
             g_ai.model_path, g_ai.in_w, g_ai.in_h, g_ai.n_output, g_ai.conf, g_ai.nms);
    return 0;

fail_buf:
    free(g_ai.in_buf);
    g_ai.in_buf = NULL;
fail_ctx:
    rknn_destroy(g_ai.ctx);
    g_ai.ctx = 0;
    LOG_ERROR("ai: init failed, AI disabled (stream continues raw)");
    return 0;
}

void rknn_yolo_destroy(void *arg)
{
    (void)arg;
    g_ai.running = 0;
    if (g_ai.ctx) {
        rknn_destroy(g_ai.ctx);
        g_ai.ctx = 0;
    }
    free(g_ai.in_buf);
    g_ai.in_buf = NULL;
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (g_ai.frame) {
        free(g_ai.frame->data);
        free(g_ai.frame);
        g_ai.frame = NULL;
    }
    pthread_mutex_unlock(&g_ai.frame_mutex);
    g_ai.enabled = 0;
}

int rknn_yolo_enabled(void)
{
    return g_ai.enabled;
}

/* 对一帧 RGB24 做 NPU 推理 + 后处理，结果存入快照 */
int rknn_yolo_detect(const unsigned char *rgb, int w, int h, unsigned long long seq)
{
    if (!g_ai.enabled || !g_ai.ctx || !g_ai.in_buf) return -1;

    rgb_resize(rgb, w, h, g_ai.in_buf, g_ai.in_w, g_ai.in_h);

    rknn_input in;
    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.type  = RKNN_TENSOR_UINT8;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.size  = (uint32_t)(g_ai.in_w * g_ai.in_h * 3);
    in.buf   = g_ai.in_buf;
    if (rknn_inputs_set(g_ai.ctx, 1, &in) != RKNN_SUCC) return -1;
    if (rknn_run(g_ai.ctx, NULL) != RKNN_SUCC) return -1;

    rknn_output outputs[AI_MAX_OUTPUTS];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < g_ai.n_output; i++) outputs[i].want_float = 1;
    if (rknn_outputs_get(g_ai.ctx, g_ai.n_output, outputs, NULL) != RKNN_SUCC) return -1;

    const float *out_buf[AI_MAX_OUTPUTS];
    for (uint32_t i = 0; i < g_ai.n_output; i++)
        out_buf[i] = (const float *)outputs[i].buf;

    yolo_result_t res;
    memset(&res, 0, sizeof(res));
    res.seq = seq;
    res.w   = w;
    res.h   = h;
    res.count = yolo_postprocess(out_buf, g_ai.out_attr, g_ai.n_output, w, h,
                                 res.dets, YOLO_MAX_DETS, g_ai.conf, g_ai.nms);

    rknn_outputs_release(g_ai.ctx, g_ai.n_output, outputs);

    pthread_mutex_lock(&g_ai.result_mutex);
    g_ai.result = res;
    pthread_mutex_unlock(&g_ai.result_mutex);
    return 0;
}

int rknn_yolo_get(yolo_result_t *out)
{
    if (!out || !g_ai.enabled) return 0;
    pthread_mutex_lock(&g_ai.result_mutex);
    *out = g_ai.result;
    pthread_mutex_unlock(&g_ai.result_mutex);
    return out->count;
}

/* 画框 + JPEG 编码，存为最新画框帧快照 */
static void render_annotated(const unsigned char *rgb, int w, int h,
                             unsigned long long seq, const yolo_result_t *res)
{
    static const unsigned int kColors[] = {
        0xFF3B30, 0x34C759, 0x007AFF, 0xFFCC00,
        0xAF52DE, 0xFF9500, 0x5AC8FA, 0xFF2D55,
        0x00C7BE, 0x8E8E93,
    };
    int ncolors = (int)(sizeof(kColors) / sizeof(kColors[0]));
    unsigned char *img = malloc((size_t)w * h * 3);
    if (!img) return;
    memcpy(img, rgb, (size_t)w * h * 3);
    for (int i = 0; i < res->count; i++) {
        const yolo_det_t *d = &res->dets[i];
        draw_box(img, w, h, (int)d->x1, (int)d->y1, (int)d->x2, (int)d->y2,
                 kColors[d->cls % ncolors]);
    }

    unsigned char *jpeg = NULL;
    size_t jlen = 0;
    if (rgb_to_jpeg(img, w, h, &jpeg, &jlen) != 0 || !jpeg) {
        free(img);
        return;
    }
    free(img);

    pthread_mutex_lock(&g_ai.frame_mutex);
    if (g_ai.frame) { free(g_ai.frame->data); free(g_ai.frame); }
    g_ai.frame = malloc(sizeof(*g_ai.frame));
    if (g_ai.frame) {
        g_ai.frame->data = jpeg;
        g_ai.frame->len  = jlen;
        g_ai.frame->seq  = seq;
        g_ai.frame->w    = w;
        g_ai.frame->h    = h;
    } else {
        free(jpeg);
    }
    pthread_mutex_unlock(&g_ai.frame_mutex);
}

int rknn_yolo_get_frame(unsigned char **data, size_t *len, unsigned long long *seq)
{
    if (!g_ai.enabled || !data || !len || !seq) return -1;
    pthread_mutex_lock(&g_ai.frame_mutex);
    if (!g_ai.frame || g_ai.frame->len == 0) {
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return -1;
    }
    unsigned char *c = malloc(g_ai.frame->len);
    if (!c) {
        pthread_mutex_unlock(&g_ai.frame_mutex);
        return -1;
    }
    memcpy(c, g_ai.frame->data, g_ai.frame->len);
    *len = g_ai.frame->len;
    *seq = g_ai.frame->seq;
    pthread_mutex_unlock(&g_ai.frame_mutex);
    *data = c;
    return 0;
}

unsigned long long rknn_yolo_get_frame_seq(void)
{
    if (!g_ai.enabled) return 0;
    pthread_mutex_lock(&g_ai.frame_mutex);
    unsigned long long s = g_ai.frame ? g_ai.frame->seq : 0;
    pthread_mutex_unlock(&g_ai.frame_mutex);
    return s;
}

/* ---- AI 工作线程：取最新帧 → 解码 → 推理 → 画框快照 ---- */
void *rknn_ai_task(void *arg)
{
    app_ctx_t *app = (app_ctx_t *)arg;
    if (!g_ai.enabled) {
        /* 未启用/降级：线程保持存活并持续喂狗（模块一致管理，无 watchdog 注册竞态），
           不占推理资源 */
        while (app->running && g_ai.running) {
            watchdog_feed_self("ai");
            usleep(200000);
        }
        return NULL;
    }
    LOG_INFO("ai: inference thread started (interval %d ms)", g_ai.interval_ms);

    while (app->running && g_ai.running) {
        watchdog_feed_self("ai");

        unsigned char *raw = NULL;
        size_t raw_len = 0;
        int fmt = 0, w = 0, h = 0;
        unsigned long long seq = 0;
        if (video_stream_get_frame(&raw, &raw_len, &fmt, &w, &h, &seq) != 0) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        if (seq == g_ai.last_seq || raw_len == 0 || w <= 0 || h <= 0) {
            free(raw);
            usleep(g_ai.interval_ms * 1000);
            continue;
        }
        g_ai.last_seq = seq;

        unsigned char *rgb = NULL;
        int rw = 0, rh = 0;
        if (fmt == RKNN_FMT_MJPEG) {
            if (jpeg_to_rgb(raw, raw_len, &rgb, &rw, &rh) != 0) { free(raw); continue; }
        } else {   /* YUYV */
            rgb = malloc((size_t)w * h * 3);
            if (rgb) { yuyv_to_rgb(raw, w, h, rgb); rw = w; rh = h; }
        }
        free(raw);
        if (!rgb) {
            usleep(g_ai.interval_ms * 1000);
            continue;
        }

        if (rknn_yolo_detect(rgb, rw, rh, seq) == 0) {
            /* detect 已写入快照；0 个目标也出画框帧（无框） */
            yolo_result_t res;
            rknn_yolo_get(&res);
            render_annotated(rgb, rw, rh, seq, &res);
        }
        free(rgb);
        usleep(g_ai.interval_ms * 1000);
    }
    return NULL;
}
