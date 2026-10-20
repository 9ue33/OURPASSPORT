#pragma once

/*
 * Heartlink 情侣心动终端 —— NVS 持久化层。
 *
 * 这一层只负责「把 couple_store_blob_t 完好地存住」，不含任何产品逻辑：
 *   - 打开命名空间 "couple"、键 "blob"，失败或版本不符时重建为出厂状态
 *   - 统计数据坏掉只重置统计，绝不因此抹掉配对关系（设计红线）
 *   - 脏标记 + 批量落盘，绝不每次按键都写 Flash
 *
 * 数据模型与清洗规则在 couple_store_blob.{h,c}（纯 C，可在宿主机测试）；
 * 真正的 Flash 读写在这里，所以这一层不做宿主测试，只在设备上验证。
 *
 * 线程模型：
 *   - 所有公开函数内部自带互斥锁，可以被任意任务调用。
 *   - 读整份数据请用 couple_store_snapshot()；couple_store() 的裸指针只给
 *     init 的同一任务用，不要跨任务读它。
 *   - couple_store_stats_update() 的回调在持锁状态下执行，必须极短：
 *     禁止 LVGL、网络、音频、long 日志、任何可能阻塞的操作。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "couple_store_blob.h"

/*
 * 幂等加载。
 *
 * 成功时内存中一定有一份可用数据（即使 NVS 打开失败也会给出厂默认值，让 UI 能起来）；
 * 返回非 ESP_OK 表示「数据没能持久化」，调用方可以据此在屏幕上提示。
 */
esp_err_t couple_store_init(void);

/* 只读视图；init 之后永不为 NULL。仅限与 init 同一任务内使用。 */
const couple_store_blob_t *couple_store(void);

/* 加锁整份拷贝，供网络、UI、配网等其它任务使用。out 不可为 NULL。 */
void couple_store_snapshot(couple_store_blob_t *out);

bool couple_store_is_paired(void);

/* 是否存有可用的 Wi-Fi 凭据（配网完成）。 */
bool couple_store_has_wifi(void);

/*
 * 取下一个同步序号。只改 RAM，不写 Flash。
 * 只有前进到 COUPLE_SEQ_PERSIST_STEP 才会标记「该落盘了」，由 tick 统一写。
 */
uint32_t couple_store_bump_seq(void);

/* 记录对端已确认的位置；只增不减，倒退的响应不会把剪枝点拉回去。 */
void couple_store_apply_ack(uint32_t ack);

/* 手动标脏（调用方只是想「稍后落一次盘」时用）。 */
void couple_store_mark_dirty(void);

/*
 * 在锁内对统计做一次读-改-写，避免两个任务互相覆盖。
 * 返回前后内容有变化时自动标脏，调用方不需要自己 mark_dirty。
 */
typedef void (*couple_store_stats_fn)(couple_stats_t *stats, void *user);
void couple_store_stats_update(couple_store_stats_fn fn, void *user);

void couple_store_stats_read(couple_stats_t *out);

/* 整体替换统计（界面上「重置数据」之类的一次性写入）。 */
void couple_store_stats_write(const couple_stats_t *in);

/* 用当前 UTC 推进「今天」的日界线；跨天时返回 true 并标脏。 */
bool couple_store_roll_day(uint32_t utc);

/* 立刻落盘。配对、解绑、深睡前必须调用。 */
esp_err_t couple_store_flush(void);

/*
 * 由常驻任务周期调用（例如首页 1 秒 tick）。
 * 达到时间间隔或脏计数上限才真正写 Flash。
 * now_ms 用同一时基的毫秒值（esp_timer_get_time()/1000 即可），回绕安全。
 */
void couple_store_tick(uint32_t now_ms);

/*
 * 写入配对结果。name 会被 couple_store_sanitize_name 清洗，可以为 NULL。
 * seq/ack 重新起步（新的 pair_id 意味着新的一段关系，旧水位无意义）。
 * 内部立即落盘——配对是极低频且必须留住的时刻。
 */
esp_err_t couple_store_set_pair(const uint8_t *pair_id, const uint8_t *pair_key,
                                const uint8_t *partner_id, const char *partner_name);

/*
 * 解除配对：清三元组、清 seq/ack，保留设备身份、Wi-Fi 与互动记忆（Love Energy 不清零）。
 * 内部立即落盘。
 */
esp_err_t couple_store_clear_pair(void);

/* 保存配网结果；ssid 为空串等价于清除凭据。内部立即落盘。 */
esp_err_t couple_store_set_wifi(const char *ssid, const char *pass);

/* 保存 relay 地址；非法地址会被拒绝（返回 ESP_ERR_INVALID_ARG，原值不动）。 */
esp_err_t couple_store_set_relay_url(const char *url);

/* 保存双方名字；任一为 NULL 表示不改。内部立即落盘。 */
esp_err_t couple_store_set_names(const char *self_name, const char *partner_name);