/**
 * frame_ring_test.c — 视频帧环形队列单元测试（build 独立 target）。
 *
 * 覆盖：槽位分配/回绕、槽满丢弃、claims 引用计数钉住、最新帧钉住、
 * AI 启停/录像启停/推理停摆四种模式组合的 raw 释放规则、
 * 显示/编码指针流转与池回收、quiesce 退让、事件等待、seq 单调。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "video/frame_ring.h"

static int g_fail = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { \
        g_fail++; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

/* ---- 模拟 V4L2 mmap 驱动缓冲与释放回调（QBUF 计数） ---- */
static unsigned char g_fake_mmap[16][4096];
static int g_qbuf_count = 0;

static void fake_release_cb(void *arg, int idx)
{
    (void)arg;
    (void)idx;
    g_qbuf_count++;
}

/* 采集一帧：从模拟驱动缓冲入槽并提交（返回 seq） */
static unsigned long long capture_one(frame_ring_t *r, int *dropped)
{
    frame_ring_lock(r);
    frame_slot_t *s = frame_ring_produce_slot_locked(r);
    if (!s) {
        frame_ring_produce_drop_locked(r);
        frame_ring_unlock(r);
        if (dropped) *dropped = 1;
        return 0;
    }
    int idx = (int)((s->seq - 1) % 16);
    s->raw.buf = g_fake_mmap[idx];
    s->raw.len = 100;
    s->raw.cap = 4096;
    s->raw_present = 1;
    s->raw_is_mmap = 1;
    s->raw_vbuf_index = idx;
    s->w = 1280; s->h = 720; s->fmt = FRAME_RING_FMT_MJPEG;
    unsigned long long seq = s->seq;
    frame_ring_produce_commit_locked(r);
    frame_ring_unlock(r);
    if (dropped) *dropped = 0;
    return seq;
}

/* 推理消费一帧（claim → 解码 → unclaim） */
static int infer_one(frame_ring_t *r, unsigned long long seq)
{
    frame_ring_lock(r);
    frame_slot_t *s = frame_ring_infer_claim_locked(r, seq);
    if (!s) { frame_ring_unlock(r); return -1; }
    unsigned char probe = s->raw.buf[0];          /* 锁外前先取个样 */
    frame_ring_unlock(r);
    (void)probe;
    usleep(1000);                                  /* 模拟锁外解码 */
    frame_ring_lock(r);
    frame_ring_infer_unclaim_locked(r, s, 1);
    frame_ring_unlock(r);
    return 0;
}

/* ---- 测试 1：基本采集/推理流转 + 最新帧钉住 + 延迟 QBUF ---- */
static void test_basic_flow(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;

    /* 默认 ai_active=0 rec_active=0：旧帧立即释放（最新帧除外） */
    unsigned long long s1 = capture_one(&r, NULL);
    CHECK(s1 == 1, "first seq = 1, got %llu", s1);
    CHECK(g_qbuf_count == 0, "newest raw pinned, qbuf=0 got %d", g_qbuf_count);

    unsigned long long s2 = capture_one(&r, NULL);
    CHECK(s2 == 2, "second seq = 2");
    CHECK(g_qbuf_count == 1, "frame1 released after frame2, qbuf=1 got %d", g_qbuf_count);
    CHECK(r.slots[1].raw_present == 0, "slot1 raw released");
    CHECK(r.slots[2].raw_present == 1, "slot2 raw still newest");

    capture_one(&r, NULL);   /* seq3：frame2 释放 */
    CHECK(g_qbuf_count == 2, "frame2 released after frame3, qbuf=2 got %d", g_qbuf_count);

    frame_ring_stats_t st;
    frame_ring_stats(&r, &st);
    CHECK(st.produce_seq == 3, "produce_seq=3 got %llu", st.produce_seq);
    CHECK(st.raw_pinned == 1, "raw_pinned=1 got %d", st.raw_pinned);

    frame_ring_destroy(&r);
}

/* ---- 测试 2：回绕（>32 帧）+ 槽描述符正确 ---- */
static void test_wraparound(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;

    for (int i = 0; i < 70; i++) {
        unsigned long long seq = capture_one(&r, NULL);
        CHECK(seq == (unsigned long long)i + 1, "seq monotonic %llu", seq);
    }
    /* 70 帧：前 69 帧已释放（最后一帧最新钉住） */
    CHECK(g_qbuf_count == 69, "69 qbuf got %d", g_qbuf_count);
    frame_ring_lock(&r);
    CHECK(r.slots[70 & 31].seq == 70, "slot of seq70 correct");
    CHECK(r.slots[1].seq == 65, "slot1 now holds seq65 (latest 32-period occupant), got %llu", r.slots[1].seq);
    frame_ring_unlock(&r);
    frame_ring_destroy(&r);
}

/* ---- 测试 3：槽满丢弃（AI 激活且停摆，无人消费） ---- */
static void test_ring_full_drop(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;
    frame_ring_set_ai_active(&r, 1);

    int dropped = 0, total_drop = 0;
    for (int i = 0; i < FRAME_RING_N + 8; i++) {
        if (capture_one(&r, &dropped) == 0) total_drop++;
    }
    CHECK(total_drop == 8, "8 drops while ring full (got %d)", total_drop);

    /* 恢复消费：全部解码后采集继续 */
    for (int i = 0; i < FRAME_RING_N; i++)
        CHECK(infer_one(&r, (unsigned long long)i + 1) == 0, "infer seq%d", i + 1);

    unsigned long long next = capture_one(&r, &dropped);
    CHECK(next == (unsigned long long)FRAME_RING_N + 1, "capture resumes at %llu",
          next);
    CHECK(dropped == 0, "no drop after consumption");

    frame_ring_stats_t st;
    frame_ring_stats(&r, &st);
    CHECK(st.capture_dropped == 8, "capture_dropped=8 got %llu", st.capture_dropped);
    frame_ring_destroy(&r);
}

/* ---- 测试 4：claims 引用计数钉住（锁外解码期间不释放） ---- */
static void test_claims_pin(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;
    /* AI 激活：raw 释放需 infer_done；claims 阻止提前释放 */
    frame_ring_set_ai_active(&r, 1);

    for (int i = 0; i < FRAME_RING_N; i++) capture_one(&r, NULL);

    /* claim seq1（模拟解码中），其余全部消费 */
    frame_ring_lock(&r);
    frame_slot_t *held = frame_ring_infer_claim_locked(&r, 1);
    CHECK(held != NULL, "claim seq1 ok");
    frame_ring_unlock(&r);
    for (int i = 2; i <= FRAME_RING_N; i++)
        CHECK(infer_one(&r, (unsigned long long)i) == 0, "infer seq%d", i);

    /* seq33 需要复用槽1：被 claims 钉住 → 丢帧 */
    int dropped = 0;
    unsigned long long seq = capture_one(&r, &dropped);
    CHECK(seq == 0 && dropped == 1, "seq33 blocked by claim (drop)");

    /* 释放 claim → 槽1 回收 → 采集恢复 */
    frame_ring_lock(&r);
    frame_ring_infer_unclaim_locked(&r, held, 1);
    frame_ring_unlock(&r);
    seq = capture_one(&r, &dropped);
    CHECK(seq == 33 && dropped == 0, "seq33 ok after unclaim, got %llu", seq);
    frame_ring_destroy(&r);
}

/* ---- 测试 5：AI 激活 + 录像激活完整流转（rgb/jpeg/nv12 指针与回池） ---- */
static void test_full_pipeline(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;
    frame_ring_set_ai_active(&r, 1);
    frame_ring_set_rec_active(&r, 1);

    /* 5 帧：采集 → 推理解码 → worker 结果入槽 */
    for (int i = 0; i < 5; i++) {
        unsigned long long seq = capture_one(&r, NULL);
        CHECK(infer_one(&r, seq) == 0, "infer seq%llu", seq);
        frame_ring_lock(&r);
        frame_slot_t *s = &r.slots[seq & 31];
        frame_ring_buf_take_locked(&r, FRAME_RING_POOL_RGB, 100, &s->rgb);
        s->res.count = 1;
        s->res.dets[0].conf = 0.9f;
        s->rgb_done = 1;
        frame_ring_signal(&r);
        frame_ring_unlock(&r);
    }

    /* 显示：最新结果优先 → 选 seq5，跳过 1-4 */
    frame_ring_lock(&r);
    frame_slot_t *d = frame_ring_display_pick_locked(&r, 0);
    CHECK(d && d->seq == 5, "display picks newest seq5");
    CHECK(d->res.dets[0].conf == 0.9f, "res snapshot intact");
    ring_buf_t nv, jp;
    frame_ring_buf_take_locked(&r, FRAME_RING_POOL_NV12, 100, &nv);
    frame_ring_buf_take_locked(&r, FRAME_RING_POOL_JPEG, 100, &jp);
    frame_ring_unlock(&r);
    usleep(1000);   /* 模拟锁外渲染 */
    frame_ring_lock(&r);
    d->nv12 = nv; d->nv12.len = 100;
    d->jpeg = jp; d->jpeg.len = 100;
    frame_ring_display_commit_locked(&r, d);
    CHECK(r.render_skipped == 4, "render_skipped=4 got %llu", r.render_skipped);
    /* 被跳过槽的 rgb 已回池（1-4 + 渲染完成的 5） */
    int rgb_left = 0;
    for (int i = 0; i < FRAME_RING_N; i++) if (r.slots[i].rgb.buf) rgb_left++;
    CHECK(rgb_left == 0, "all rgb released, got %d left", rgb_left);
    /* jpeg：仅最新显示帧钉住 */
    CHECK(d->jpeg.buf != NULL, "seq5 jpeg pinned");
    frame_ring_unlock(&r);

    /* 编码：rec 窃取 nv12 → 用毕回池 */
    frame_ring_lock(&r);
    frame_slot_t *e = frame_ring_encode_pick_locked(&r, 0);
    CHECK(e && e->seq == 5, "encode picks newest displayed seq5");
    unsigned char *stolen = e->nv12.buf;
    size_t stolen_cap = e->nv12.cap;
    e->nv12.buf = NULL; e->nv12.len = e->nv12.cap = 0;
    frame_ring_encode_advance_locked(&r, e);
    CHECK(e->encode_done == 1, "encode_done set");
    /* 槽 1-4 被跳过 → 一并标记（其 raw 已可释放） */
    CHECK(r.slots[3].encode_done == 1, "skipped slot encode_done");
    frame_ring_unlock(&r);
    usleep(1000);   /* 模拟锁外编码 */
    frame_ring_lock(&r);
    frame_ring_buf_put_locked(&r, FRAME_RING_POOL_NV12, stolen, stolen_cap);
    frame_ring_unlock(&r);

    /* 帧 6 到达：帧 5 不再是最新，raw 释放（infer_done + 非最新） */
    capture_one(&r, NULL);
    CHECK(g_qbuf_count == 5, "5 raws released after seq6, got %d", g_qbuf_count);

    frame_ring_stats_t st;
    frame_ring_stats(&r, &st);
    CHECK(st.pool_nv12 == 1, "stolen nv12 returned to pool (got %d)", st.pool_nv12);
    CHECK(st.pool_jpeg == 0, "jpeg pinned in display slot, pool empty (got %d)",
          st.pool_jpeg);
    frame_ring_destroy(&r);
}

/* ---- 测试 6：AI 停用 + 录像激活（raw 回退拷贝 + 跳过即消费） ---- */
static void test_raw_fallback(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;
    /* ai_active=0（默认），rec_active=1：raw 释放需 encode_done */
    frame_ring_set_rec_active(&r, 1);

    for (int i = 0; i < 3; i++) capture_one(&r, NULL);

    /* rec 回退：锁内拷贝最新帧 → 标记消费（旧槽一并跳过） */
    frame_ring_lock(&r);
    frame_slot_t *s = frame_ring_raw_newest_locked(&r);
    CHECK(s && s->seq == 3, "raw newest seq3");
    CHECK(s->raw.buf != NULL, "raw present for copy");
    frame_ring_encode_mark_locked(&r, s, 1);   /* ai_stalled=1 */
    CHECK(r.infer_stalled == 1, "infer_stalled set");
    frame_ring_unlock(&r);
    /* 帧 1、2 已被跳过标记 → 释放；帧 3 最新钉住 */
    CHECK(g_qbuf_count == 2, "frames 1-2 released, qbuf=2 got %d", g_qbuf_count);

    /* 帧 4：帧 3 释放 */
    capture_one(&r, NULL);
    CHECK(g_qbuf_count == 3, "frame3 released, qbuf=3 got %d", g_qbuf_count);

    /* AI 恢复：成功取帧自动清除 infer_stalled */
    frame_ring_lock(&r);
    frame_slot_t *c = frame_ring_infer_claim_locked(&r, 4);
    CHECK(c != NULL, "claim seq4 after recovery");
    CHECK(r.infer_stalled == 0, "infer_stalled cleared on claim");
    frame_ring_infer_unclaim_locked(&r, c, 1);
    frame_ring_unlock(&r);

    /* 停录：nv12/raw 立即回收 */
    frame_ring_set_rec_active(&r, 0);
    frame_ring_destroy(&r);
}

/* ---- 测试 7：quiesce 退让（claims 归零前阻塞、槽清空、seq 单调） ---- */
static void *hold_claim_thread(void *arg)
{
    frame_ring_t *r = (frame_ring_t *)arg;
    /* 默认模式下旧帧早已释放，最新帧（seq10）被钉住 → claim 它 */
    frame_ring_lock(r);
    frame_slot_t *s = frame_ring_infer_claim_locked(r, 10);
    frame_ring_unlock(r);
    CHECK(s != NULL, "helper claim seq10");
    usleep(150000);   /* 持锁外引用 150ms */
    frame_ring_lock(r);
    if (s) frame_ring_infer_unclaim_locked(r, s, 1);
    frame_ring_unlock(r);
    return NULL;
}

static void test_quiesce(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;

    for (int i = 0; i < 10; i++) capture_one(&r, NULL);
    /* 默认模式旧帧立即释放：帧 1-9 已 QBUF，帧 10 钉住 */
    int qbuf_before = g_qbuf_count;
    CHECK(qbuf_before == 9, "9 released before quiesce, got %d", qbuf_before);

    /* helper 先 claim seq10（持有 150ms），quiesce 后须等其归零 */
    pthread_t th;
    pthread_create(&th, NULL, hold_claim_thread, &r);
    usleep(30000);   /* 确保 helper 已 claim */

    frame_ring_quiesce_begin(&r);
    /* quiesce 后新 claim 被拒绝 */
    frame_ring_lock(&r);
    CHECK(frame_ring_infer_claim_locked(&r, 10) == NULL, "claim rejected while quiescing");
    frame_ring_unlock(&r);

    int rc = frame_ring_quiesce_wait(&r, 1000);
    pthread_join(th, NULL);
    CHECK(rc == 0, "quiesce wait succeeds after claims drain");
    /* 槽全部清空，seq 保持单调 */
    frame_ring_lock(&r);
    for (int i = 0; i < FRAME_RING_N; i++)
        CHECK(r.slots[i].raw_present == 0 && r.slots[i].seq == 0,
              "slot%d cleared", i);
    CHECK(r.produce_seq == 10, "produce_seq monotonic after quiesce, got %llu",
          r.produce_seq);
    CHECK(g_qbuf_count == qbuf_before, "quiesce drops mmap silently (no QBUF), got %d",
          g_qbuf_count);
    frame_ring_unlock(&r);

    /* 重启后继续采集：seq 从 11 继续 */
    unsigned long long seq = capture_one(&r, NULL);
    CHECK(seq == 11, "seq continues at 11, got %llu", seq);
    frame_ring_destroy(&r);

    /* 超时路径：claim 永不归零 → -1 */
    frame_ring_t r2;
    frame_ring_init(&r2);
    frame_ring_set_raw_release_cb(&r2, fake_release_cb, NULL);
    capture_one(&r2, NULL);
    frame_ring_lock(&r2);
    frame_slot_t *held = frame_ring_infer_claim_locked(&r2, 1);
    frame_ring_unlock(&r2);
    frame_ring_quiesce_begin(&r2);
    rc = frame_ring_quiesce_wait(&r2, 150);
    CHECK(rc == -1, "quiesce timeout returns -1");
    frame_ring_lock(&r2);
    frame_ring_infer_unclaim_locked(&r2, held, 1);
    frame_ring_unlock(&r2);
    frame_ring_destroy(&r2);
}

/* ---- 测试 8：事件等待（wait_new / wait_render） ---- */
static void *producer_thread(void *arg)
{
    frame_ring_t *r = (frame_ring_t *)arg;
    for (int i = 0; i < 5; i++) {
        usleep(20000);
        unsigned long long seq = capture_one(r, NULL);
        if (!seq) continue;
        frame_ring_lock(r);
        frame_slot_t *s = &r->slots[seq & 31];
        frame_ring_buf_take_locked(r, FRAME_RING_POOL_RGB, 100, &s->rgb);
        s->rgb_done = 1;
        frame_ring_signal(r);
        frame_ring_unlock(r);
    }
    return NULL;
}

static void test_event_wait(void)
{
    frame_ring_t r;
    frame_ring_init(&r);

    /* 空环：wait_new 超时返回 0 */
    CHECK(frame_ring_wait_new(&r, 0, 100) == 0, "wait_new timeout on empty ring");

    pthread_t th;
    pthread_create(&th, NULL, producer_thread, &r);
    CHECK(frame_ring_wait_new(&r, 0, 1000) == 1, "wait_new wakes on produce");
    CHECK(frame_ring_wait_render(&r, 0, 1000) == 1, "wait_render wakes on rgb_done");
    pthread_join(th, NULL);

    frame_ring_stats_t st;
    frame_ring_stats(&r, &st);
    CHECK(st.produce_seq == 5, "5 frames produced, got %llu", st.produce_seq);
    frame_ring_destroy(&r);
}

/* ---- 测试 9：缓冲池复用 ---- */
static void test_pool_reuse(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_lock(&r);
    ring_buf_t a, b;
    frame_ring_buf_take_locked(&r, FRAME_RING_POOL_RGB, 100, &a);
    CHECK(a.buf != NULL && a.cap == 100, "pool allocates");
    unsigned char *p1 = a.buf;
    frame_ring_buf_put_locked(&r, FRAME_RING_POOL_RGB, a.buf, a.cap);
    frame_ring_buf_take_locked(&r, FRAME_RING_POOL_RGB, 100, &b);
    CHECK(b.buf == p1, "same buffer reused from pool");
    frame_ring_buf_put_locked(&r, FRAME_RING_POOL_RGB, b.buf, b.cap);

    /* 容量不足：分配新缓冲，旧的留在池内 */
    ring_buf_t c;
    frame_ring_buf_take_locked(&r, FRAME_RING_POOL_RGB, 100000, &c);
    CHECK(c.buf != p1 && c.cap == 100000, "larger request allocates fresh");
    frame_ring_buf_put_locked(&r, FRAME_RING_POOL_RGB, c.buf, c.cap);
    frame_ring_unlock(&r);
    frame_ring_destroy(&r);
}

/* ---- 测试 10：推理停摆时旧槽由编码路径释放（防 32 槽耗尽） ---- */
static void test_ai_stall_release(void)
{
    frame_ring_t r;
    frame_ring_init(&r);
    frame_ring_set_raw_release_cb(&r, fake_release_cb, NULL);
    g_qbuf_count = 0;
    frame_ring_set_ai_active(&r, 1);   /* AI 激活但停摆：无人 claim */
    frame_ring_set_rec_active(&r, 1);

    for (int i = 0; i < 4; i++) capture_one(&r, NULL);
    /* rec 检测停摆 → 回退拷贝最新帧并置 stalled */
    frame_ring_lock(&r);
    frame_slot_t *s = frame_ring_raw_newest_locked(&r);
    CHECK(s && s->seq == 4, "raw newest seq4");
    frame_ring_encode_mark_locked(&r, s, 1);
    frame_ring_unlock(&r);
    /* 帧 1-3 经 stalled 路径释放（encode_done 跳过标记），帧 4 钉住 */
    CHECK(g_qbuf_count == 3, "stalled path releases frames 1-3, got %d", g_qbuf_count);
    capture_one(&r, NULL);   /* 帧 5 → 帧 4 释放 */
    CHECK(g_qbuf_count == 4, "frame4 released, got %d", g_qbuf_count);

    /* AI 恢复：帧 1-4 槽已被编码路径释放 → claim 全部失败（跳过），
       最新帧 5 仍钉住 → claim 成功并自动清除 infer_stalled */
    frame_ring_lock(&r);
    CHECK(frame_ring_infer_claim_locked(&r, 1) == NULL, "released slot claim fails");
    CHECK(frame_ring_infer_claim_locked(&r, 4) == NULL, "stalled-consumed slot claim fails");
    frame_slot_t *c = frame_ring_infer_claim_locked(&r, 5);
    CHECK(c != NULL, "claim seq5 ok");
    CHECK(r.infer_stalled == 0, "infer_stalled cleared on claim");
    frame_ring_infer_unclaim_locked(&r, c, 1);
    frame_ring_unlock(&r);
    frame_ring_destroy(&r);
}

int main(void)
{
    test_basic_flow();
    test_wraparound();
    test_ring_full_drop();
    test_claims_pin();
    test_full_pipeline();
    test_raw_fallback();
    test_quiesce();
    test_event_wait();
    test_pool_reuse();
    test_ai_stall_release();

    printf("frame_ring_test: %d checks, %d failure(s)\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
