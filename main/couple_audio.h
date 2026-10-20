#pragma once

/*
 * Heartlink 情侣心动终端 —— 音效播放 worker。
 *
 * 本模块是 bsp_audio 的唯一所有者：初始化 codec/I2S、设定采样格式与音量、以及
 * 所有 PCM 写入，都只发生在它自己的 worker 任务里。其它模块只投递「播哪个音效」，
 * 不直接调用 bsp_audio_*，因此不存在多任务同时操作 codec 的竞态，
 * 也就不会踩到 bsp_audio_set_format 文档里「调用方须串行化」那条约束。
 *
 * 对外接口全部非阻塞（只入队或只改一个 32 位标志），
 * 因此在 LVGL 回调或按键回调里调用是安全的。
 *
 * 播放走 couple_tone 的流式渲染：静态缓冲只有 2 KB，最长的 2400 ms 里程碑音
 * 也是分块拉出来写 I2S，不会为了一个音效常驻 76 KB DRAM。
 * 分块粒度为 COUPLE_AUDIO_CHUNK_SAMPLES（64 ms），所以「停止/取消」的最坏延迟
 * 就是一个块的时长。
 *
 * 唯一需要阻塞的接口是 couple_audio_quiesce()，它只为深睡前收尾准备，
 * 绝不允许在回调或 LVGL 上下文中调用。
 */

#include "couple_tone.h"
#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * 初始化音频通道：内部依次调用 bsp_audio_init()、bsp_audio_set_format(16k/16bit/单声道)
 * 和 bsp_audio_set_volume(COUPLE_AUDIO_VOLUME)，然后创建 worker 任务。
 *
 * 幂等：已就绪时直接返回 ESP_OK。失败时不留下半初始化状态，修正外部故障后可以重试。
 * 调用方不需要（也不应该）自己再调用 bsp_audio_*。
 */
esp_err_t couple_audio_init(void);

/* 音频通道是否就绪。未就绪时所有播放入口都静默失败，UI 不受影响。 */
bool couple_audio_ready(void);

/*
 * 入队播放一次音效。队列满或音效无效时返回 false。
 * 返回 true 只表示「已入队」，不表示已经播完。
 */
bool couple_audio_play(couple_tone_t tone);

/*
 * 循环播放某个音效（双向抱抱的同步心跳用），两拍之间自动插入
 * COUPLE_AUDIO_LOOP_GAP_MS 的静音。同一时刻至多一个循环音效：
 * 重复调用等于替换。
 *
 * 循环期间入队的 couple_audio_play() 音效仍会照常插播，
 * 播完自动回到循环。
 */
bool couple_audio_loop(couple_tone_t tone);

/* 是否正在循环。 */
bool couple_audio_loop_active(void);

/* 停止循环（最坏延迟一个分块）。已入队的单次音效不受影响。 */
void couple_audio_stop_loop(void);

/*
 * 立即取消：停掉循环、截断正在播的分块、并丢弃队列里所有已排队的音效。
 * 「解绑」这类要让设备马上安静的场合用它。
 */
void couple_audio_cancel(void);

/* 调整输出音量（0..100，超出范围会被夹紧）。在 worker 里生效，避免与播放争 codec。 */
bool couple_audio_set_volume(uint8_t percent);

/* 是否正在写 I2S（含循环之间的静音间隔）。 */
bool couple_audio_playing(void);

/*
 * 取消并等待通道安静下来，最多等 timeout_ms 毫秒。
 * 返回 true 表示已静音；false 表示超时（此时 I2S 可能仍在写，不要直接进深睡）。
 *
 * 只能在普通任务里调用，不能在音频 worker 自己或任何回调里调用。
 */
bool couple_audio_quiesce(uint32_t timeout_ms);