#include "couple_tone.h"

/*
 * 情侣心动终端的合成音效。
 *
 * 全部用整数定点运算现场生成：相位为 32 位累加器（一圈 = 2^32），幅度为 Q15，
 * 正弦由四分之一周期查表 + 线性插值得到，因此不需要 <math.h>，宿主机与
 * RISC-V 目标机的结果逐样本一致，也不会把 libm 拖进固件。
 *
 * 每个声部（voice）是一段频率固定的正弦（可选第二分音、可选滑音、可选颤音），
 * 各自带一条解析包络：攻击 -> 保持 -> 衰减 -> 释放。包络的首尾样本恒为 0，
 * 所以每段音效都从零起、回零落，直接写 I2S 不会爆音。
 *
 * 渲染是 tone 的纯函数：没有动态分配，没有可变静态状态，也没有 VLA，
 * 同样的 tone 渲染两次字节完全一致。
 *
 * 一次性渲染与流式渲染（couple_tone_stream_t）共用同一个逐样本核心
 * tone_render_core()，只是状态由谁携带不同：前者用栈上的临时状态跑一遍，
 * 后者把状态留在调用方对象里，好让几 KB 缓冲的音频 worker 分块拉取最长
 * 76800 字节的 PCM。两条路径逐字节一致。
 */

#define TONE_FLAG_CUBIC 0x01u   /* 衰减段走三次曲线，更接近自然衰减 */
#define TONE_FLAG_VIBRATO 0x02u /* 轻微颤音 */

/* 单个音效的声部上限（HEART_SYNC 用满 7 个）。 */
#define VOICE_MAX 7u

/* 四分之一周期正弦表：sin(pi/2 * i / 64) * 32767，i = 0..64。 */
static const int16_t kSineQ[65] = {
        0,    804,   1608,   2410,   3212,   4011,   4808,   5602,
     6393,   7179,   7962,   8739,   9512,  10278,  11039,  11793,
    12539,  13279,  14010,  14732,  15446,  16151,  16846,  17530,
    18204,  18868,  19519,  20159,  20787,  21403,  22005,  22594,
    23170,  23731,  24279,  24811,  25329,  25832,  26319,  26790,
    27245,  27683,  28105,  28510,  28898,  29268,  29621,  29956,
    30273,  30571,  30852,  31113,  31356,  31580,  31785,  31971,
    32137,  32285,  32412,  32521,  32609,  32678,  32728,  32757,
    32767
};

typedef struct {
    uint16_t freq;     /* 基频 Hz */
    uint16_t freq2;    /* 第二分音 Hz，0 表示没有 */
    uint16_t freq_end; /* 滑音终点 Hz，0 表示不滑音 */
    int16_t amp;       /* 基频峰值幅度 */
    int16_t amp2;      /* 第二分音峰值幅度 */
    uint16_t start;    /* 起始样本 */
    uint16_t atk;      /* 攻击段样本数 */
    uint16_t hold;     /* 满幅保持样本数 */
    uint16_t dec;      /* 衰减段样本数 */
    uint16_t rel;      /* 释放段样本数，总长 = atk+hold+dec+rel+1 */
    int16_t sus;       /* 衰减段结束时的电平（Q15） */
    uint8_t flags;
} tone_voice_t;

typedef struct {
    uint16_t total; /* 总样本数 */
    uint8_t first;  /* kVoices 中的起始下标 */
    uint8_t count;
} tone_def_t;

/*
 * 音高（整数 Hz，十二平均律，宿主值四舍五入）：
 * G3=196  A5=880  C6=1047  D6=1175  E6=1319  G6=1568  C7=2093  E7=2637
 * 分音取整数倍（440 = 2×220、128 = 2×64、4704 = 3×1568）或非谐倍率（5650 ≈ 2.7×2093）。
 *
 * 常用包络形状：
 *   轻快短音   atk 32 / hold 1100 / rel 307            len 1440（90 ms）
 *   八分短音   atk 32 / hold 1000 / rel 247            len 1280（80 ms）
 *   琶音音     atk 32 / hold  920 / rel 247            len 1200（75 ms）
 *   和弦尾音   atk 400 / hold 4600 / dec 2000 / rel 1639（三次衰减）
 *   心跳闷响   atk 24 / dec 2694 / rel 1                len 2720（170 ms，三次衰减到零）
 *   低沉蜂鸣   atk 48 / hold 700 / dec 1165 / rel 326    len 2240（140 ms，三次衰减到 sus）
 */
static const tone_voice_t kVoices[] = {
    /*  freq    f2  fend     amp   amp2  start   atk  hold    dec    rel    sus  flags */
    /* MISS_SEND：A5 -> D6，两声轻快上行 */
    {   880,    0,    0,  22000,  4000,     0,   32, 1100,     0,   307, 32767, 0 },
    {  1175,    0,    0,  22000,  4000,  1520,   32, 1100,     0,   307, 32767, 0 },
    /* MISS_RECV：C6 -> E6 -> G6，末音带一点高次闪烁分音 */
    {  1047,    0,    0,  22000,     0,     0,   32, 1000,     0,   247, 32767, 0 },
    {  1319,    0,    0,  22000,     0,  1360,   32, 1000,     0,   247, 32767, 0 },
    {  1568, 4704,    0,  22000,  4000,  2720,   32, 1000,     0,   247, 32767, 0 },
    /* HEART_SYNC：C6 -> E6 -> G6 -> C7 琶音，接 C 大三和弦长尾 */
    {  1047,    0,    0,  17000,     0,     0,   32, 1100,     0,   307, 32767, 0 },
    {  1319,    0,    0,  17000,     0,  1440,   32, 1100,     0,   307, 32767, 0 },
    {  1568,    0,    0,  17000,     0,  2880,   32, 1100,     0,   307, 32767, 0 },
    {  2093,    0,    0,  17000,     0,  4320,   32, 1100,     0,   307, 32767, 0 },
    {  1047,    0,    0,   8000,     0,  5760,  400, 4600,  2000,  1639, 10000, TONE_FLAG_CUBIC },
    {  1319,    0,    0,   8000,     0,  5760,  400, 4600,  2000,  1639, 10000, TONE_FLAG_CUBIC },
    {  1568,    0,    0,   8000,     0,  5760,  400, 4600,  2000,  1639, 10000, TONE_FLAG_CUBIC },
    /* HUG_START：220 Hz 暖音，第二分音补一点厚度 */
    {   220,  440,    0,  21000,  2600,     0, 1600, 3200,     0,  2399, 32767, 0 },
    /* HUG_END：330 Hz -> 220 Hz 柔和下行滑音 */
    {   330,    0,  220,  20000,     0,     0,  480, 2000,     0,  1359, 32767, 0 },
    /* HUG_SYNC：两下心跳闷响，t=0 与 t=260 ms */
    {    64,  128,    0,  21000,  4000,     0,   24,    0,  2694,     1,     0, TONE_FLAG_CUBIC },
    {    64,  128,    0,  21000,  4000,  4160,   24,    0,  2694,     1,     0, TONE_FLAG_CUBIC },
    /* HEARTBEAT：单下闷响，后面是静音，便于 worker 无缝循环 */
    {    64,  128,    0,  21000,  4000,     0,   24,    0,  2694,     1,     0, TONE_FLAG_CUBIC },
    /* MILESTONE：C6 E6 G6 C7 E7 五音上行 + 钟形余韵 */
    {  1047,    0,    0,  21000,     0,     0,   32,  920,     0,   247, 32767, 0 },
    {  1319,    0,    0,  21000,     0,  1200,   32,  920,     0,   247, 32767, 0 },
    {  1568,    0,    0,  21000,     0,  2400,   32,  920,     0,   247, 32767, 0 },
    {  2093,    0,    0,  21000,     0,  3600,   32,  920,     0,   247, 32767, 0 },
    {  2637,    0,    0,  21000,     0,  4800,   32,  920,     0,   247, 32767, 0 },
    {  2093, 5650,    0,  19000,  4000,  6000,  320,  800,  7000,  1879,     0, TONE_FLAG_CUBIC },
    /* MILESTONE_BIG：带颤音的慢速上行琶音 + 两个相差 5 Hz 的高钟音 */
    {  1047,    0,    0,  12000,     0,     0,   64, 4000,     0,  1135, 32767, TONE_FLAG_VIBRATO },
    {  1319,    0,    0,  12000,     0,  4000,   64, 4000,     0,  1135, 32767, TONE_FLAG_VIBRATO },
    {  1568,    0,    0,  12000,     0,  8000,   64, 4000,     0,  1135, 32767, TONE_FLAG_VIBRATO },
    {  2093,    0,    0,  12000,     0, 12000,   64, 4000,     0,  1135, 32767, TONE_FLAG_VIBRATO },
    {  2093, 2098,    0,  13000, 11000, 17600, 1600, 2400, 16000,   799,  1200, TONE_FLAG_CUBIC },
    /* ERROR：两声低沉蜂鸣（G3=196 Hz，带 2 倍分音），各 140 ms，首尾相接不重叠 */
    {   196,  392,    0,  21000,  4000,     0,   48,  700,  1165,   326,  7000, TONE_FLAG_CUBIC },
    {   196,  392,    0,  21000,  4000,  2240,   48,  700,  1165,   326,  7000, TONE_FLAG_CUBIC }
};

/* kVoices 中的声部总数，供 tone_valid 做表自洽检查。 */
#define TONE_VOICE_COUNT (sizeof(kVoices) / sizeof(kVoices[0]))

static const tone_def_t kTones[COUPLE_TONE_COUNT] = {
    {     0,  0, 0 }, /* COUPLE_TONE_NONE */
    {  3200,  0, 2 }, /* COUPLE_TONE_MISS_SEND     200 ms */
    {  4480,  2, 3 }, /* COUPLE_TONE_MISS_RECV     280 ms */
    { 14400,  5, 7 }, /* COUPLE_TONE_HEART_SYNC    900 ms */
    {  7200, 12, 1 }, /* COUPLE_TONE_HUG_START     450 ms */
    {  3840, 13, 1 }, /* COUPLE_TONE_HUG_END       240 ms */
    {  7360, 14, 2 }, /* COUPLE_TONE_HUG_SYNC      460 ms */
    { 11520, 16, 1 }, /* COUPLE_TONE_HEARTBEAT     720 ms */
    { 16000, 17, 6 }, /* COUPLE_TONE_MILESTONE    1000 ms */
    { 38400, 23, 5 }, /* COUPLE_TONE_MILESTONE_BIG 2400 ms */
    {  4480, 28, 2 }  /* COUPLE_TONE_ERROR         280 ms */
};

static int tone_valid(couple_tone_t tone)
{
    unsigned id = (unsigned)tone;

    if (id <= (unsigned)COUPLE_TONE_NONE || id >= (unsigned)COUPLE_TONE_COUNT) {
        return 0;
    }
    /*
     * kTones 与 kVoices 必须自洽：声部数不超过 VOICE_MAX，且声部区间落在
     * kVoices 之内。表写错时宁可判为无效音效，也不要越界读表。
     */
    if ((unsigned)kTones[id].count > VOICE_MAX) {
        return 0;
    }
    if ((unsigned)kTones[id].first + (unsigned)kTones[id].count > TONE_VOICE_COUNT) {
        return 0;
    }
    return 1;
}

/* 相位 -> Q15 正弦；phase 一圈为 2^32。 */
static int32_t tone_sine_q15(uint32_t phase)
{
    uint32_t ang = phase >> 16; /* 0..65535 */
    int32_t neg = 0;

    if (ang >= 32768u) {
        ang -= 32768u;
        neg = 1;
    }
    if (ang >= 16384u) {
        ang = 32768u - ang; /* 折回 [0, 16384] */
    }

    uint32_t idx = ang >> 8;
    int32_t frac = (int32_t)(ang & 0xFFu);
    if (idx >= 64u) {
        idx = 63u;
        frac = 256;
    }
    int32_t v = (int32_t)kSineQ[idx] +
                (((int32_t)kSineQ[idx + 1u] - (int32_t)kSineQ[idx]) * frac) / 256;
    return neg ? -v : v;
}

/* 每样本相位步进（一圈 = 2^32）：f / 16000 * 2^32 = f * 268435.456。 */
static int32_t tone_inc(uint32_t hz)
{
    return (int32_t)(hz * 268435u) + (int32_t)((hz * 456u + 500u) / 1000u);
}

/* Q15 乘法，(a * b) / 32768，只用 32 位中间结果。 */
static int32_t tone_mul_q15(int32_t a, int32_t b)
{
    return (int32_t)(((int64_t)a * (int64_t)b) / 32768);
}

/*
 * 声部包络（Q15）。k 为声部内偏移，len = atk+hold+dec+rel+1。
 * k == 0 与 k == len-1 恒为 0，因此音效首尾样本精确为零。
 */
static int32_t tone_env(const tone_voice_t *v, uint32_t k, uint32_t len)
{
    uint32_t atk = v->atk ? v->atk : 1u;
    uint32_t hold = v->hold;
    uint32_t dec = v->dec;
    uint32_t rel = v->rel;

    if (k == 0u || k + 1u >= len) {
        return 0;
    }
    if (k <= atk) {
        return (int32_t)((32767u * k) / atk);
    }
    if (k <= atk + hold) {
        return 32767;
    }
    if (dec != 0u && k <= atk + hold + dec) {
        uint32_t u = ((dec - (k - atk - hold)) * 32768u) / dec; /* 32768 -> 0 */
        int32_t f = (int32_t)u;
        if ((v->flags & TONE_FLAG_CUBIC) != 0u) {
            f = (((f * f) / 32768) * f) / 32768;
        }
        return v->sus + tone_mul_q15(32767 - v->sus, f);
    }
    if (rel <= 1u) {
        return 0;
    }
    return ((int32_t)v->sus * (int32_t)(rel - (k - atk - hold - dec))) / (int32_t)(rel - 1u);
}

/*
 * 把流置到「该音效第 0 个样本之前」的初始状态，声部初始化与逐样本推进所需
 * 的常量（相位步进、滑音步进、颤音深度）在这里一次算好。
 *
 * 传 COUPLE_TONE_NONE 即空闲态：kTones[NONE] 是 {0,0,0}，所以 tone、voice_count、
 * total 全为 0，render/remaining 自然都返回 0，无需另写一份清零代码。
 * 未使用的声部槽位也一并归零，begin 之后结构体内容只取决于 tone，便于断言。
 */
static void tone_stream_state_init(couple_tone_stream_t *st, couple_tone_t tone)
{
    const tone_def_t *def = &kTones[tone];

    st->tone = (uint8_t)tone;
    st->voice_count = def->count;
    st->voice_first = def->first;
    st->reserved0 = 0u;
    st->pos = 0u;
    st->total = def->total;

    for (uint32_t v = 0; v < COUPLE_TONE_VOICE_MAX; ++v) {
        const tone_voice_t *voice = (v < (uint32_t)def->count) ? &kVoices[def->first + v] : 0;

        st->ph[v] = 0u;
        st->ph2[v] = 0u;
        st->lfo[v] = 0u;
        st->inc[v] = 0;
        st->inc2[v] = 0;
        st->ramp[v] = 0;
        st->ramp_step[v] = 0;
        st->vib[v] = 0;
        if (voice == 0) {
            continue;
        }

        st->inc[v] = tone_inc(voice->freq);
        st->inc2[v] = voice->freq2 ? tone_inc(voice->freq2) : 0;
        /* 线性滑音：在一个声部内把相位步进均匀推到终点频率，带 Q16 小数位。 */
        if (voice->freq_end != 0u) {
            uint32_t vlen = (uint32_t)voice->atk + voice->hold + voice->dec + voice->rel + 1u;
            int64_t delta = (int64_t)tone_inc(voice->freq_end) - (int64_t)tone_inc(voice->freq);
            st->ramp_step[v] = (delta * 65536) / (int64_t)vlen;
            if (st->ramp_step[v] == 0 && delta != 0) {
                st->ramp_step[v] = delta > 0 ? 1 : -1;
            }
        }
        /* 颤音深度取基频的约 0.45%（5 Hz 低频调制）。 */
        st->vib[v] = (voice->flags & TONE_FLAG_VIBRATO) != 0u ? st->inc[v] / 220 : 0;
    }
}

/*
 * 逐样本合成核心：渲染 [from, to) 这段样本，写进 out[0 .. to-from)，所有跨样本
 * 状态都留在 st 里，因此调用方可以从任意位置续渲染。
 *
 * 一次性渲染与流式渲染共用这一份数学，所以任何分块方式都逐字节一致。
 *
 * fade 只服务于一种情况：一次性渲染被 out_cap 截断，需要把截断前缀的末尾压回
 * 零。流式渲染必须传 0——每一块都做淡出会在块与块的交界处留台阶，拼起来就不再
 * 等于整段；而且音效本身的包络已保证首尾样本为 0，本来也不需要淡出。
 */
static void tone_render_core(couple_tone_stream_t *st, size_t from, size_t to,
                             uint32_t fade, int16_t *out)
{
    for (size_t i = from; i < to; ++i) {
        int32_t acc = 0;

        for (uint32_t v = 0; v < st->voice_count; ++v) {
            const tone_voice_t *voice = &kVoices[st->voice_first + v];
            if (i < (size_t)voice->start) {
                continue;
            }
            uint32_t k = (uint32_t)(i - (size_t)voice->start);
            uint32_t vlen = (uint32_t)voice->atk + voice->hold + voice->dec + voice->rel + 1u;
            if (k >= vlen) {
                continue;
            }

            int32_t rate = st->inc[v];
            if (st->ramp_step[v] != 0) {
                st->ramp[v] += st->ramp_step[v];
                rate += (int32_t)(st->ramp[v] / 65536);
            }
            if (st->vib[v] != 0) {
                rate += tone_mul_q15(tone_sine_q15(st->lfo[v]), st->vib[v]);
                st->lfo[v] += tone_inc(5u);
            }
            int32_t s = tone_sine_q15(st->ph[v]);
            st->ph[v] += (uint32_t)rate;

            int32_t e = tone_env(voice, k, vlen);
            if (e != 0) {
                acc += tone_mul_q15(tone_mul_q15(e, s), voice->amp);
                if (voice->freq2 != 0u) {
                    acc += tone_mul_q15(tone_mul_q15(e, tone_sine_q15(st->ph2[v])), voice->amp2);
                }
            }
            if (voice->freq2 != 0u) {
                st->ph2[v] += (uint32_t)st->inc2[v];
            }
        }

        if (fade != 0u && i + (size_t)fade >= to) {
            acc = tone_mul_q15(acc, (int32_t)(((to - 1u - i) * 32768u) / (size_t)fade));
        }
        if (acc > 32767) {
            acc = 32767;
        } else if (acc < -32767) {
            acc = -32767;
        }
        out[i - from] = (int16_t)acc;
    }
}

size_t couple_tone_total_samples(couple_tone_t tone)
{
    if (!tone_valid(tone)) {
        return 0u;
    }
    return (size_t)kTones[tone].total;
}

uint32_t couple_tone_duration_ms(couple_tone_t tone)
{
    size_t total = couple_tone_total_samples(tone);

    if (total == 0u) {
        return 0u;
    }
    return (uint32_t)((total * 1000u + (COUPLE_TONE_SAMPLE_RATE / 2u)) / COUPLE_TONE_SAMPLE_RATE);
}

size_t couple_tone_stream_begin(couple_tone_stream_t *st, couple_tone_t tone)
{
    if (st == 0) {
        return 0u;
    }
    if (!tone_valid(tone)) {
        /* 无效音效：置为空闲态，调用方后续的 render 一律返回 0。 */
        tone_stream_state_init(st, COUPLE_TONE_NONE);
        return 0u;
    }

    tone_stream_state_init(st, tone);
    return (size_t)kTones[tone].total;
}

size_t couple_tone_stream_render(couple_tone_stream_t *st, int16_t *out, size_t out_cap)
{
    if (st == 0 || out == 0 || out_cap == 0u) {
        return 0u;
    }
    /* 空闲或已渲染完：不写、不推进，直接返回 0。 */
    if (st->tone == (uint8_t)COUPLE_TONE_NONE || st->pos >= st->total) {
        return 0u;
    }

    size_t remain = (size_t)(st->total - st->pos);
    size_t n = out_cap < remain ? out_cap : remain;

    /* 流式路径绝不淡出：见 tone_render_core() 的说明。 */
    tone_render_core(st, (size_t)st->pos, (size_t)st->pos + n, 0u, out);
    st->pos += (uint32_t)n;
    return n;
}

size_t couple_tone_stream_remaining(const couple_tone_stream_t *st)
{
    if (st == 0 || st->pos >= st->total) {
        return 0u;
    }
    return (size_t)(st->total - st->pos);
}

size_t couple_tone_render(couple_tone_t tone, int16_t *out, size_t out_cap)
{
    if (out == 0 || out_cap == 0u || !tone_valid(tone)) {
        return 0u;
    }

    /*
     * 一次性渲染 = 流式渲染的第一块：状态仍放在栈上，只是不由调用方持有。
     * 这样两条路径共用同一份初始化与逐样本数学，截断淡出也照旧。
     */
    couple_tone_stream_t st;
    size_t total = couple_tone_stream_begin(&st, tone);
    size_t n = out_cap < total ? out_cap : total;

    /* out_cap 截断时只保留前缀，因此给尾部叠一段短释放，保证末样本为零。 */
    uint32_t fade = 0u;
    if (n < total) {
        fade = (uint32_t)(n / 4u);
        if (fade > 128u) {
            fade = 128u;
        }
        if (fade < 1u) {
            fade = 1u;
        }
    }

    tone_render_core(&st, 0u, n, fade, out);
    return n;
}
