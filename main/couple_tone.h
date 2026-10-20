#pragma once

/*
 * Heartlink 情侣心动终端 —— 合成音效。
 *
 * 纯 C，不依赖 ESP-IDF，也不依赖 LVGL，可以在宿主机上直接编译和测试。
 *
 * 音效全部由几何波形 + 包络现场合成，不使用任何解码器与音频资源文件：
 * 这样既不占 Flash，也不需要额外的托管组件，还能保证每一段都以零样本
 * 起止，避免 I2S 直写时的爆音。
 *
 * 格式固定为 16 kHz / 16 bit / 单声道（与 AI PASSPORT 的 ES8311 通路一致）。
 */

#include <stddef.h>
#include <stdint.h>

#define COUPLE_TONE_SAMPLE_RATE 16000u

typedef enum {
    COUPLE_TONE_NONE = 0,
    COUPLE_TONE_MISS_SEND,      /* 发出想你：两音轻快上行 */
    COUPLE_TONE_MISS_RECV,      /* 收到想你：三音绽开 */
    COUPLE_TONE_HEART_SYNC,     /* 心有灵犀：明亮琶音 + 尾音 */
    COUPLE_TONE_HUG_START,      /* 开始抱抱：柔和低音 */
    COUPLE_TONE_HUG_END,        /* 松开抱抱：短促下行 */
    COUPLE_TONE_HUG_SYNC,       /* 双向抱抱确认：两下心跳 */
    COUPLE_TONE_HEARTBEAT,      /* 单次心跳，由音频 worker 循环播放 */
    COUPLE_TONE_MILESTONE,      /* 里程碑彩蛋 */
    COUPLE_TONE_MILESTONE_BIG,  /* 520 / 1314 专属浪漫长音 */
    COUPLE_TONE_ERROR,          /* 操作失败 */
    COUPLE_TONE_COUNT
} couple_tone_t;

/* 该音效的样本数（16 kHz 下的长度）。无效音效返回 0。 */
size_t couple_tone_total_samples(couple_tone_t tone);

/* 该音效的时长（毫秒）。无效音效返回 0。 */
uint32_t couple_tone_duration_ms(couple_tone_t tone);

/*
 * 渲染音效到 out，最多写 out_cap 个样本。
 * 返回写入的样本数；out_cap 小于音效长度时只写前缀（仍保持零起止）。
 * tone 无效或 out 为 NULL 时返回 0。
 *
 * out 内容为 int16，取值范围限定在 [-32767, 32767]，不会溢出或截断失真。
 */
size_t couple_tone_render(couple_tone_t tone, int16_t *out, size_t out_cap);

/*
 * ---------------------------------------------------------------------------
 * 流式渲染
 * ---------------------------------------------------------------------------
 *
 * 最长音效 MILESTONE_BIG 有 38400 个样本（76800 字节 PCM），而音频 worker 的
 * 静态缓冲只有几 KB，把整段 PCM 常驻 DRAM 并不现实。所以把逐样本累加的状态
 * （相位、分音相位、颤音相位、滑音累加量）显式放进下面这个结构体，调用方就能
 * 以任意分块大小把整段拉出来，且与一次性渲染逐字节一致。
 *
 * 结构体只有几百字节，可以直接做栈对象或静态对象；内部不做任何动态分配。
 * 典型用法：begin -> render 若干次 -> 渲染返回 0 即结束；同一对象可以再次 begin。
 */

/* 内部声部上限 VOICE_MAX 的公开镜像，供调用方声明结构体。 */
#define COUPLE_TONE_VOICE_MAX 7u

typedef struct {
    uint8_t  tone;        /* 0 = 空闲/已结束，否则是正在播放的 couple_tone_t */
    uint8_t  voice_count; /* 当前音效的声部数 */
    uint8_t  voice_first; /* 内部声部表中的起始下标 */
    uint8_t  reserved0;
    uint32_t pos;         /* 下一个待渲染样本的下标 */
    uint32_t total;       /* 当前音效的总样本数 */
    uint32_t ph[COUPLE_TONE_VOICE_MAX];
    uint32_t ph2[COUPLE_TONE_VOICE_MAX];
    uint32_t lfo[COUPLE_TONE_VOICE_MAX];
    int32_t  inc[COUPLE_TONE_VOICE_MAX];
    int32_t  inc2[COUPLE_TONE_VOICE_MAX];
    int64_t  ramp[COUPLE_TONE_VOICE_MAX];
    int64_t  ramp_step[COUPLE_TONE_VOICE_MAX];
    int32_t  vib[COUPLE_TONE_VOICE_MAX];
} couple_tone_stream_t;

/*
 * 开始流式渲染 tone。返回该音效的总样本数；tone 无效时返回 0，
 * 此时 *st 处于空闲态，后续 render 也返回 0。
 */
size_t couple_tone_stream_begin(couple_tone_stream_t *st, couple_tone_t tone);

/*
 * 从 st->pos 继续，最多再写 out_cap 个样本，且不做截断淡出。
 * 返回写入的样本数（空闲或已结束时为 0）；out 为 NULL 或 out_cap 为 0 时
 * 返回 0 且不推进进度。
 */
size_t couple_tone_stream_render(couple_tone_stream_t *st, int16_t *out, size_t out_cap);

/*
 * 尚未渲染的样本数（空闲或已结束时为 0）；传入空闲流也只是返回 0。
 */
size_t couple_tone_stream_remaining(const couple_tone_stream_t *st);
