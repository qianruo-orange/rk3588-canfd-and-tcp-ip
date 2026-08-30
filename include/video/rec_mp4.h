/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef REC_MP4_H
#define REC_MP4_H

#include <stddef.h>
#include <stdint.h>

/**
 * video/rec_mp4.h — 最小 MP4(H.265 hvc1 track) 封装器。
 *
 * 文件布局（ISO BMFF，moov 末尾回写，通用播放器可播）：
 *   ftyp(28) + mdat(8+N 帧 H.265 length-prefixed) + moov
 *   stbl 记录每帧绝对偏移（stco）/ 大小（stsz）/ 平均帧间隔（stts），
 *   stss 记录关键帧（IRAP），stsd 用 hvc1+hvcC（VPS/SPS/PPS）。
 *
 * 纯封装，无线程；调用方保证并发安全。
 */

typedef struct rec_mp4_s rec_mp4_t;

/* 创建会话：目录建文件，文件名为 <prefix>_YYYYMMDD_HHMMSS.mp4（写入 name_out）。
   失败返回 NULL（目录不可写 / 内存不足）。 */
rec_mp4_t *rec_mp4_create(const char *dir, const char *prefix, int w, int h,
                          char *name_out, size_t name_size);

/* 追加一帧 H.265（Annex-B，由封装器转为 length-prefixed 写入 mdat）。
   @keyframe 1 表示 IRAP（写入 stss）。成功返回 0。
   达上限（帧数 / mdat 体积，防 32 位溢出）返回 -1（自动触发停止录制）。 */
int rec_mp4_write_frame(rec_mp4_t *s, const unsigned char *hevc, size_t len,
                        int keyframe, uint64_t ts_ms);

/* 结束会话：回填 mdat size + 写 moov + 关文件。
   @return 0 成功；-1 失败（空录制无帧时删除残缺文件） */
int rec_mp4_finalize(rec_mp4_t *s);

/* 会话信息（录制状态展示用） */
uint32_t    rec_mp4_frames(const rec_mp4_t *s);
uint32_t    rec_mp4_bytes(const rec_mp4_t *s);
uint64_t    rec_mp4_start_ms(const rec_mp4_t *s);

#endif /* REC_MP4_H */
