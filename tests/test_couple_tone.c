/*
 * couple_tone 的宿主测试：不依赖 ESP-IDF / LVGL，也不需要 libm。
 * 逐音效验证长度、零起止、幅度范围、峰值电平、直流漂移、可重复性与截断行为。
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "couple_tone.h"

/* MILESTONE_BIG 是最长音效（38400 样本），缓冲区按它开。 */
#define TONE_BUF_MAX 38400u
#define TONE_GUARD 12345

/*
 * 直流阈值：|均值| <= 峰值 / 16（约满幅的 6%）。单个不平衡的半周期最多贡献
 * 「峰值 / 周期数」量级的直流；而真正有害的直流应当接近满幅量级，会顶掉
 * 动态范围。实测各音效均值都远小于该阈值。
 */
#define TONE_DC_DIVISOR 16

static int16_t g_a[TONE_BUF_MAX + 2u];
static int16_t g_b[TONE_BUF_MAX + 2u];
static int g_failed;

static void fill_guard(int16_t *buf, size_t n, int16_t value)
{
    for (size_t i = 0; i < n; ++i) {
        buf[i] = value;
    }
}

static void check(int ok, const char *what, couple_tone_t tone, unsigned long value)
{
    if (!ok) {
        printf("FAIL %s: tone=%d value=%lu\n", what, (int)tone, value);
        g_failed = 1;
    }
}

/*
 * int16_t 永远不可能超过 32767，所以「超出 [-32767, 32767]」在本类型上只剩
 * 「等于 -32768」一种可能；这里直接按该唯一情况判断，避免无意义的越界比较。
 */
static int samples_in_range(const int16_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == INT16_MIN) {
            return 0;
        }
    }
    return 1;
}

int main(void)
{
    /* 1. 无效音效：NONE、越界值、以及负值转换来的枚举值。 */
    couple_tone_t bad[4];
    bad[0] = COUPLE_TONE_NONE;
    bad[1] = (couple_tone_t)COUPLE_TONE_COUNT;
    bad[2] = (couple_tone_t)(COUPLE_TONE_COUNT + 7);
    bad[3] = (couple_tone_t)-1;
    for (unsigned i = 0; i < 4u; ++i) {
        check(couple_tone_total_samples(bad[i]) == 0, "bad_total", bad[i], 0);
        check(couple_tone_duration_ms(bad[i]) == 0, "bad_duration", bad[i], 0);
        check(couple_tone_render(bad[i], g_a, 64u) == 0, "bad_render", bad[i], 0);
    }

    couple_tone_t longest = COUPLE_TONE_NONE;
    size_t longest_total = 0;

    for (int id = (int)COUPLE_TONE_NONE + 1; id < (int)COUPLE_TONE_COUNT; ++id) {
        couple_tone_t tone = (couple_tone_t)id;
        size_t total = couple_tone_total_samples(tone);
        uint32_t ms = couple_tone_duration_ms(tone);

        /* 2. 长度与时长。 */
        check(total > 0u, "total_nonzero", tone, (unsigned long)total);
        check(ms >= 100u, "duration_min", tone, (unsigned long)ms);
        /* 时长 = 样本数 * 1000 / 16000，四舍五入到最近毫秒。 */
        int64_t scaled = (int64_t)ms * 16000;
        int64_t exact = (int64_t)total * 1000;
        check(scaled <= exact + 8000 && scaled > exact - 8000, "duration_round", tone, (unsigned long)ms);

        /* 3. 完整渲染：长度、零起止、值域、峰值电平。 */
        fill_guard(g_a, TONE_BUF_MAX + 2u, TONE_GUARD);
        size_t n = couple_tone_render(tone, g_a, TONE_BUF_MAX);
        check(n == total, "render_len", tone, (unsigned long)n);
        check(g_a[0] == 0, "first_sample_zero", tone, (unsigned)g_a[0]);
        check(g_a[n - 1u] == 0, "last_sample_zero", tone, (unsigned)g_a[n - 1u]);

        int32_t peak = 0;
        int64_t sum = 0;
        for (size_t i = 0; i < n; ++i) {
            int32_t s = g_a[i];
            sum += s;
            if (s < 0) {
                s = -s;
            }
            if (s > peak) {
                peak = s;
            }
        }
        check(samples_in_range(g_a, n), "sample_range", tone, 0);
        check(peak > 1000, "not_silent", tone, (unsigned long)peak);
        /* 峰值落在满幅的 20% ~ 80% 之间。 */
        check((int64_t)peak * 100 >= 20 * 32767 && (int64_t)peak * 100 <= 80 * 32767,
              "peak_level", tone, (unsigned long)peak);
        /* 均值远小于峰值，避免直流偏置吃掉动态范围。 */
        check(sum * TONE_DC_DIVISOR <= (int64_t)peak * (int64_t)n &&
              sum * TONE_DC_DIVISOR >= -(int64_t)peak * (int64_t)n,
              "dc_drift", tone, (unsigned long)(sum < 0 ? -sum : sum));

        /* 4. 可重复性：同样输入渲染两次，字节完全一致。 */
        fill_guard(g_b, TONE_BUF_MAX + 2u, (int16_t)(TONE_GUARD + 1));
        size_t n2 = couple_tone_render(tone, g_b, TONE_BUF_MAX);
        check(n2 == n && memcmp(g_a, g_b, n * sizeof(int16_t)) == 0, "determinism", tone, 0);

        /* 5. 截断：返回写入数、末样本归零、不越界写入。 */
        size_t caps[3];
        caps[0] = 1u;
        caps[1] = 37u;
        caps[2] = total - 1u;
        for (unsigned c = 0; c < 3u; ++c) {
            size_t cap = caps[c];
            fill_guard(g_a, cap + 1u, TONE_GUARD);
            size_t got = couple_tone_render(tone, g_a, cap);
            check(got == cap, "trunc_len", tone, (unsigned long)got);
            check(g_a[cap - 1u] == 0, "trunc_last_zero", tone, (unsigned)g_a[cap - 1u]);
            check(g_a[cap] == TONE_GUARD, "trunc_overwrite", tone, (unsigned)g_a[cap]);
            check(samples_in_range(g_a, cap), "trunc_range", tone, 0);
        }

        /* 6. 空参数。 */
        check(couple_tone_render(tone, NULL, 128u) == 0, "null_out", tone, 0);
        check(couple_tone_render(tone, g_a, 0u) == 0, "zero_cap", tone, 0);

        if (total > longest_total) {
            longest_total = total;
            longest = tone;
        }
    }

    /* 7. MILESTONE_BIG 必须是最长音效。 */
    check(longest == COUPLE_TONE_MILESTONE_BIG, "longest_tone", longest, (unsigned long)longest_total);

    /* 8. HEARTBEAT 固定 720 ms，且闷响之后确实是静音。 */
    couple_tone_t hb = COUPLE_TONE_HEARTBEAT;
    size_t hb_total = couple_tone_total_samples(hb);
    check(hb_total == 720u * 16u, "heartbeat_length", hb, (unsigned long)hb_total);
    check(couple_tone_duration_ms(hb) == 720u, "heartbeat_duration", hb, couple_tone_duration_ms(hb));
    if (couple_tone_render(hb, g_a, hb_total) == hb_total) {
        size_t tenth = hb_total / 10u;
        int64_t head = 0;
        int64_t tail = 0;
        for (size_t i = 0; i < tenth; ++i) {
            head += g_a[i] < 0 ? -g_a[i] : g_a[i];
        }
        for (size_t i = hb_total / 2u; i < hb_total; ++i) {
            tail += g_a[i] < 0 ? -g_a[i] : g_a[i];
        }
        check(head / (int64_t)tenth > 1000, "heartbeat_head_loud", hb, (unsigned long)(head / (int64_t)tenth));
        /* 后半段要比前十分之一安静得多（闷响已衰减到零）。 */
        check((tail / (int64_t)tenth) * 8 < head / (int64_t)tenth, "heartbeat_decay", hb,
              (unsigned long)(tail / (int64_t)tenth));
    } else {
        check(0, "heartbeat_render", hb, 0);
    }

    /*
     * 9. 流式渲染：任意切块都必须与一次性渲染逐字节一致。
     *    1 是极限切块（每个样本一次调用）；37 与 999 都除不尽各音效长度，
     *    用来考验分块边界上的中间状态；7000 是大块，用来考验跨块携带状态。
     */
    static const size_t stream_chunks[] = { 1u, 37u, 999u, 7000u };

    for (int id = (int)COUPLE_TONE_NONE + 1; id < (int)COUPLE_TONE_COUNT; ++id) {
        couple_tone_t tone = (couple_tone_t)id;
        size_t total = couple_tone_total_samples(tone);
        couple_tone_stream_t st;

        /* 参考波形：一次性渲染整段。 */
        fill_guard(g_a, TONE_BUF_MAX + 2u, TONE_GUARD);
        size_t ref = couple_tone_render(tone, g_a, TONE_BUF_MAX);
        check(ref == total, "stream_ref_len", tone, (unsigned long)ref);

        for (unsigned c = 0; c < sizeof(stream_chunks) / sizeof(stream_chunks[0]); ++c) {
            size_t chunk = stream_chunks[c];

            check(couple_tone_stream_begin(&st, tone) == total, "stream_begin_len", tone,
                  (unsigned long)st.total);
            check(couple_tone_stream_remaining(&st) == total, "stream_begin_remaining", tone,
                  (unsigned long)st.pos);

            fill_guard(g_b, TONE_BUF_MAX + 2u, TONE_GUARD);
            size_t off = 0;
            while (off < total) {
                size_t want = chunk < total - off ? chunk : total - off;
                /* 每次调用前，剩余样本数都应当等于「总长 - 已写入」。 */
                check(couple_tone_stream_remaining(&st) == total - off, "stream_remaining_step", tone,
                      (unsigned long)off);
                size_t got = couple_tone_stream_render(&st, g_b + off, chunk);
                off += got;
                check(got == want, "stream_chunk_len", tone, (unsigned long)got);
                check(couple_tone_stream_remaining(&st) == total - off, "stream_remaining_after", tone,
                      (unsigned long)off);
                if (got == 0u) {
                    break; /* 防御：若未按预期推进就不要死循环。 */
                }
            }
            check(off == total, "stream_written", tone, (unsigned long)off);
            check(couple_tone_stream_remaining(&st) == 0, "stream_end_remaining", tone, (unsigned long)off);
            /* 结束之后再渲染必须返回 0，且不再写任何样本。 */
            check(couple_tone_stream_render(&st, g_b + off, chunk) == 0, "stream_end_render", tone,
                  (unsigned long)off);

            /* 逐字节一致：这是流式渲染唯一可接受的验收标准。 */
            check(memcmp(g_a, g_b, total * sizeof(int16_t)) == 0, "stream_identical", tone,
                  (unsigned long)chunk);
            check(g_b[0] == 0, "stream_first_zero", tone, (unsigned)g_b[0]);
            check(g_b[total - 1u] == 0, "stream_last_zero", tone, (unsigned)g_b[total - 1u]);
            check(g_b[total] == TONE_GUARD, "stream_overwrite", tone, (unsigned)g_b[total]);
        }

        /* 空参数：返回 0 且不推进进度。 */
        check(couple_tone_stream_begin(&st, tone) == total, "stream_arg_begin", tone, (unsigned long)st.total);
        check(couple_tone_stream_render(&st, NULL, 64u) == 0, "stream_null_out", tone, 0);
        check(couple_tone_stream_render(&st, g_b, 0u) == 0, "stream_zero_cap", tone, 0);
        check(couple_tone_stream_remaining(&st) == total, "stream_arg_no_advance", tone, (unsigned long)st.pos);
    }

    /* 10. 无效音效让流保持空闲；空闲流（含全零结构体）渲染与剩余都是 0。 */
    for (unsigned i = 0; i < 4u; ++i) {
        couple_tone_stream_t st;
        check(couple_tone_stream_begin(&st, bad[i]) == 0, "stream_bad_begin", bad[i], 0);
        check(st.tone == 0u, "stream_bad_idle", bad[i], (unsigned long)st.tone);
        check(couple_tone_stream_remaining(&st) == 0, "stream_bad_remaining", bad[i], 0);
        check(couple_tone_stream_render(&st, g_b, 64u) == 0, "stream_bad_render", bad[i], 0);
    }
    {
        couple_tone_stream_t st;
        memset(&st, 0, sizeof(st)); /* 全零即空闲态。 */
        check(couple_tone_stream_remaining(&st) == 0, "stream_zero_remaining", COUPLE_TONE_NONE, 0);
        check(couple_tone_stream_render(&st, g_b, 64u) == 0, "stream_zero_render", COUPLE_TONE_NONE, 0);
    }

    if (g_failed != 0) {
        puts("FAIL couple tone host tests");
        return 1;
    }
    puts("PASS couple tone host tests");
    return 0;
}
