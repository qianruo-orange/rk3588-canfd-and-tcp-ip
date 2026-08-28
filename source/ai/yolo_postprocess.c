/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * yolo_postprocess.c — YOLO26 单输出后处理（官方 ultralytics rknn 导出格式）。
 *
 * 模型输出 (1, 4+nc, A) NCHW 或 (1, A, 4+nc) NHWC，A 为全部锚点数（640 输入时 8400）：
 *   框通道已是像素坐标 cx,cy,w,h（模型内完成解码），
 *   分数通道已 sigmoid，板端只需阈值过滤 + 类别内 NMS。
 * 纯函数，线程安全。
 */

#include <math.h>
#include <string.h>

#include "ai/yolo_postprocess.h"

/* NCHW 快速路径的逐锚点临时缓冲容量（覆盖 640 输入的 8400 锚点） */
#define YOLO_MAX_ANCHORS 16384

/* 解析单输出扁平格式：dims 中应恰好有两个 >1 的维度（batch 维为 1），
   较小者为通道数 ch（4+nc），较大者为锚点数 A。
   返回 ch/A 并标记 NCHW（ch 维在 A 维之前）或 NHWC。 */
static int post_single_head(const rknn_tensor_attr *a, int *nchw_out,
                            int *ch_out, int *A_out)
{
    uint32_t nd = a->n_dims;
    uint32_t d[2] = { 0, 0 }, idx[2] = { 0, 0 };
    int n = 0;
    for (uint32_t i = 0; i < nd && n < 2; i++) {
        uint32_t v = a->dims[i];
        if (v <= 1) continue;
        d[n] = v;
        idx[n] = i;
        n++;
    }
    if (n != 2) return -1;
    int ch = d[0] < d[1] ? (int)d[0] : (int)d[1];
    int A  = d[0] < d[1] ? (int)d[1] : (int)d[0];
    if (ch < 5 || ch > 300) return -1;              /* 通道维明显异常：不是检测输出 */
    if ((uint32_t)ch * (uint32_t)A != a->n_elems) return -1;
    *nchw_out = d[0] < d[1] ? (idx[0] < idx[1]) : (idx[1] < idx[0]);
    *ch_out = ch;
    *A_out = A;
    return 0;
}

/* 置信度降序 + 类别内 NMS + 输出 dets */
static int yolo_nms_finish(float *cand, int ncand, yolo_det_t *dets,
                           int max_dets, float nms)
{
    int ndet = 0;
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

    /* NMS（类别内抑制；栈上分配避免多线程竞争） */
    char suppressed[YOLO_MAX_DETS];
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

int yolo_postprocess(const float *const *out_buf, const rknn_tensor_attr *attrs,
                     uint32_t n_output, int in_w, int in_h,
                     int frame_w, int frame_h,
                     yolo_det_t *dets, int max_dets,
                     float conf, float nms, int *nc_out)
{
    if (n_output != 1) {
        if (nc_out) *nc_out = 0;
        return 0;   /* 模型不匹配：上层已记录日志 */
    }
    float sx = (float)frame_w / (float)in_w;
    float sy = (float)frame_h / (float)in_h;
    /* 候选上限即数组容量：YOLO_MAX_DETS（勿用 sizeof(cand)/6，那是字节数） */
    float cand[YOLO_MAX_DETS * 6];   /* x1,y1,x2,y2,score,cls */
    int cand_cap = YOLO_MAX_DETS;
    int ncand = 0;

    int nchw = 0, ch = 0, A = 0;
    if (post_single_head(&attrs[0], &nchw, &ch, &A) != 0) {
        if (nc_out) *nc_out = 0;
        return 0;
    }
    int nc = ch - 4;
    if (nc_out) *nc_out = nc > 0 ? nc : 80;
    if (nc <= 0) nc = 80;

    const float *buf = out_buf[0];

    /* NCHW：按类别顺序扫描（每类一行连续访问），逐锚点记录最高分与类别，
       避免逐锚点跨类扫描的 33KB 跨步缓存缺失（实测 15ms → ~1ms） */
    static __thread float best[YOLO_MAX_ANCHORS];
    static __thread int   bestc[YOLO_MAX_ANCHORS];

    if (nchw) {
        if (A > YOLO_MAX_ANCHORS) {
            /* 兜底：锚点数超出快速缓冲容量（非常规输入尺寸），逐锚点扫描 */
            for (int a_i = 0; a_i < A && ncand < cand_cap; a_i++) {
                int bi = 0;
                float bests = -1.0f;
                for (int c = 0; c < nc; c++) {
                    float s = buf[(size_t)(4 + c) * A + a_i];
                    if (s > bests) { bests = s; bi = c; }
                }
                if (bests < conf) continue;
                float cx = buf[(size_t)0 * A + a_i], cy = buf[(size_t)1 * A + a_i];
                float bw = buf[(size_t)2 * A + a_i], bh = buf[(size_t)3 * A + a_i];
                cand[ncand * 6 + 0] = (cx - bw * 0.5f) * sx;
                cand[ncand * 6 + 1] = (cy - bh * 0.5f) * sy;
                cand[ncand * 6 + 2] = (cx + bw * 0.5f) * sx;
                cand[ncand * 6 + 3] = (cy + bh * 0.5f) * sy;
                cand[ncand * 6 + 4] = bests;
                cand[ncand * 6 + 5] = (float)bi;
                ncand++;
            }
            return yolo_nms_finish(cand, ncand, dets, max_dets, nms);
        }
        for (int a_i = 0; a_i < A; a_i++) best[a_i] = -1.0f;
        for (int c = 0; c < nc; c++) {
            const float *src = buf + (size_t)(4 + c) * A;
            for (int a_i = 0; a_i < A; a_i++) {
                float s = src[a_i];
                if (s > best[a_i]) { best[a_i] = s; bestc[a_i] = c; }
            }
        }
        for (int a_i = 0; a_i < A && ncand < cand_cap; a_i++) {
            if (best[a_i] < conf) continue;
            float cx = buf[(size_t)0 * A + a_i], cy = buf[(size_t)1 * A + a_i];
            float bw = buf[(size_t)2 * A + a_i], bh = buf[(size_t)3 * A + a_i];
            cand[ncand * 6 + 0] = (cx - bw * 0.5f) * sx;
            cand[ncand * 6 + 1] = (cy - bh * 0.5f) * sy;
            cand[ncand * 6 + 2] = (cx + bw * 0.5f) * sx;
            cand[ncand * 6 + 3] = (cy + bh * 0.5f) * sy;
            cand[ncand * 6 + 4] = best[a_i];
            cand[ncand * 6 + 5] = (float)bestc[a_i];
            ncand++;
        }
        return yolo_nms_finish(cand, ncand, dets, max_dets, nms);
    }

    /* NHWC：行内通道连续，逐锚点扫描即可 */
    for (int a_i = 0; a_i < A && ncand < cand_cap; a_i++) {
        const float *row = buf + (size_t)a_i * ch;
        /* 分数已 sigmoid：直接取类别最大值 */
        int bi = 0;
        float bests = -1.0f;
        for (int c = 0; c < nc; c++) {
            float s = row[4 + c];
            if (s > bests) { bests = s; bi = c; }
        }
        if (bests < conf) continue;
        float cx = row[0], cy = row[1], bw = row[2], bh = row[3];
        cand[ncand * 6 + 0] = (cx - bw * 0.5f) * sx;
        cand[ncand * 6 + 1] = (cy - bh * 0.5f) * sy;
        cand[ncand * 6 + 2] = (cx + bw * 0.5f) * sx;
        cand[ncand * 6 + 3] = (cy + bh * 0.5f) * sy;
        cand[ncand * 6 + 4] = bests;
        cand[ncand * 6 + 5] = (float)bi;
        ncand++;
    }
    return yolo_nms_finish(cand, ncand, dets, max_dets, nms);
}
