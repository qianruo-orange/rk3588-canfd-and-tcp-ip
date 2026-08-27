/**
 * yolo_postprocess.c — YOLO26 三输出模式后处理（YOLOv8 风格）。
 * 模型须为经典三检测头（P3/P4/P5），共 3 个输出：
 *   各输出 [1, 4+nc, H, W]（NCHW）或 [1, H*W, 4+nc]（NHWC），
 *   4+nc 为原始 logits，需 sigmoid 后按网格 stride 解码，再做类别内 NMS。
 * 纯函数，线程安全。
 */

#include <math.h>
#include <string.h>

#include "ai/yolo_postprocess.h"

static float sigmoid(float x)
{
    return 1.0f / (1.0f + expf(-x));
}

int yolo_postprocess(const float *const *out_buf, const rknn_tensor_attr *attrs,
                     uint32_t n_output, int in_w, int in_h,
                     int frame_w, int frame_h,
                     yolo_det_t *dets, int max_dets,
                     float conf, float nms, int *nc_out)
{
    if (n_output != 3) {
        if (nc_out) *nc_out = 0;
        return 0;   /* 非三输出模型不匹配：上层已记录日志 */
    }
    float sx = (float)frame_w / (float)in_w;
    float sy = (float)frame_h / (float)in_h;
    int ndet = 0;
    /* 候选上限即数组容量：YOLO_MAX_DETS（勿用 sizeof(cand)/6，那是字节数） */
    float cand[YOLO_MAX_DETS * 6];   /* x1,y1,x2,y2,score,cls */
    int cand_cap = YOLO_MAX_DETS;
    int ncand = 0;

    /* 类别数：所有检测头通道维一致，取 ch-4（布局由 dims 启发式推断，不依赖 fmt） */
    int nc = 0;
    for (uint32_t o = 0; o < n_output; o++) {
        uint32_t nd = attrs[o].n_dims;
        uint32_t ch = 0;
        if (nd == 4)                                       ch = attrs[o].dims[1];
        else if (nd == 3 && attrs[o].dims[1] < attrs[o].dims[2]) ch = attrs[o].dims[1];
        else if (nd == 3)                                  ch = attrs[o].dims[2];
        else return 0;
        if (ch > (uint32_t)nc) nc = (int)ch;
    }
    nc -= 4;
    if (nc_out) *nc_out = nc > 0 ? nc : 80;
    if (nc <= 0) nc = 80;

    for (uint32_t o = 0; o < n_output; o++) {
        const rknn_tensor_attr *a = &attrs[o];
        const float *buf = out_buf[o];
        uint32_t nd = a->n_dims;
        uint32_t ch = 0, H = 0, W = 0;
        int nchw = 0;
        if (nd == 4) { nchw = 1; ch = a->dims[1]; H = a->dims[2]; W = a->dims[3]; }
        else if (nd == 3 && a->dims[1] < a->dims[2]) { nchw = 1; ch = a->dims[1]; H = a->dims[2]; W = a->dims[3] ? a->dims[3] : 1; }
        else if (nd == 3) { ch = a->dims[2]; W = a->dims[1]; H = 1; }
        else return 0;
        if (ch < 5 || H == 0 || W == 0) continue;
        uint32_t A = H * W;
        if ((uint64_t)A * ch != a->n_elems) continue;
        int stride = (int)roundf((float)in_w / (float)W);
        if (stride <= 0) stride = 8;
        for (uint32_t a_i = 0; a_i < A; a_i++) {
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
            if (ncand < cand_cap) {
                cand[ncand * 6 + 0] = (x - bw * 0.5f) * sx; cand[ncand * 6 + 1] = (y - bh * 0.5f) * sy;
                cand[ncand * 6 + 2] = (x + bw * 0.5f) * sx; cand[ncand * 6 + 3] = (y + bh * 0.5f) * sy;
                cand[ncand * 6 + 4] = bests; cand[ncand * 6 + 5] = (float)best;
                ncand++;
            } else {
                /* 满员：新候选分高于当前最低分则替换（最终排序在 NMS 前统一做） */
                int p = 0;
                for (int k = 1; k < cand_cap; k++)
                    if (cand[k * 6 + 4] < cand[p * 6 + 4]) p = k;
                if (bests > cand[p * 6 + 4]) {
                    cand[p * 6 + 0] = (x - bw * 0.5f) * sx; cand[p * 6 + 1] = (y - bh * 0.5f) * sy;
                    cand[p * 6 + 2] = (x + bw * 0.5f) * sx; cand[p * 6 + 3] = (y + bh * 0.5f) * sy;
                    cand[p * 6 + 4] = bests; cand[p * 6 + 5] = (float)best;
                }
            }
        }
    }
    /* 按置信度降序排列（贪心 NMS 前提，避免低分框抑制高分框） */
    for (int i = 1; i < ncand; i++) {
        float t[6];
        memcpy(t, &cand[i * 6], sizeof(t));
        int j = i - 1;
        while (j >= 0 && cand[j * 6 + 4] < t[4]) {
            memcpy(&cand[(j + 1) * 6], &cand[j * 6], sizeof(t));
            j--;
        }
        memcpy(&cand[(j + 1) * 6], t, sizeof(t));
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
}
