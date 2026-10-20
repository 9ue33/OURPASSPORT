#pragma once

/*
 * Heartlink 情侣心动终端 —— 关系状态、Love Energy、每日系统与本地文案格式化。
 *
 * 纯 C，不依赖 ESP-IDF，也不依赖 LVGL，可以在宿主机上直接编译和测试。
 * 整个结构体是一个可以直接落 NVS 的平坦 POD，不含指针，方便批量保存。
 *
 * ===================== 计分规则（唯一权威定义） =====================
 *
 *   想你（发出）        Love Energy +1
 *   想你（收到）        Love Energy +1
 *   心有灵犀            Love Energy +5
 *   双向抱抱            Love Energy +5
 *   连续每日互动        首次互动时结算，续上连续天数 +3
 *   里程碑彩蛋          跨越时一次性发放 9/18/99/100/365/520/999/1314
 *
 * 「互动总数」只统计真实的想你与抱抱：
 *   想你（任一方向）+1；抱抱结束且达到「小抱抱」档位 +1（任一方向）。
 *   没到档位的误触不算互动，连当日首次互动与连续天数都不记。
 *   心有灵犀与双向抱抱只加能量、不重复计入互动数（它们本来就是同一次互动的确认）。
 *
 * 抱抱不按次单独发能量：抱抱的价值体现在「双向抱抱 HUG_SYNC +5」上。
 * 单方面按住只会让对方屏幕上多一个抱抱，凑次数不会换来能量。
 *
 * 每日卡片是「我们俩的一天」：day_misses 与 day_hugs 都统计双向，
 * 所以两台设备上显示的是同一个数字。
 *
 * 设计红线：不施加压力。日界刷新只清空当日计数，
 * 断联、隔天、连续天数归零都不产生任何责备性文案或惩罚。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "couple_types.h"

/* ---------- 版本 ---------- */

#define COUPLE_STATE_MAGIC 0x484C5354u /* "HLST" */
#define COUPLE_STATE_SCHEMA_VERSION 1u

/* ---------- 关系统计 ---------- */

typedef struct {
    uint32_t magic; /* COUPLE_STATE_MAGIC，加载时校验 */
    uint16_t schema_version;
    uint16_t reserved0;

    uint32_t energy;             /* Love Energy 累计值 */
    uint32_t total_interactions; /* 想你 + 抱抱 的累计次数 */

    uint32_t misses_sent;
    uint32_t misses_received;
    uint32_t hugs_shared;  /* 双向抱抱次数 */
    uint32_t heart_syncs;  /* 心有灵犀次数 */

    uint32_t last_miss_received_utc; /* 最近一次收到想你的 UTC 秒；0 = 还没收到过 */

    uint32_t last_unix_time; /* 最近一次已知的 UTC 秒（由 relay 对时得来）；0 = 未对时 */
    uint32_t day_start_utc;  /* 当前统计日的本地日界线（UTC 表示） */

    uint32_t day_first_interaction_utc; /* 今天首次互动的 UTC 秒；0 = 今天还没互动 */
    uint16_t day_misses;                /* 今日想你次数（双向） */
    uint16_t day_hugs;                  /* 今日抱抱次数（双向） */

    uint16_t streak_days;        /* 当前连续互动天数 */
    uint16_t last_interaction_day; /* 最近一次互动的本地日序号；0 = 从未 */

    uint32_t milestones_seen; /* bit i 置位 = 里程碑 i 的彩蛋已经展示过 */
} couple_stats_t;

/* 初始化为空关系（magic 与 schema_version 一并写好）。 */
void couple_state_init(couple_stats_t *stats);

/* magic 与 schema 都匹配才返回 true。加载 NVS 后必须先过这一关。 */
bool couple_state_valid(const couple_stats_t *stats);

/* ---------- 日界线 ---------- */

/* 本地日序号（1970-01-01 本地时间为第 0 天）。 */
uint32_t couple_day_index(uint32_t utc, int32_t tz_offset_minutes);

/* 当前本地日界线，用 UTC 秒表示。 */
uint32_t couple_day_start_utc(uint32_t utc, int32_t tz_offset_minutes);

/*
 * 若 utc 已经跨到新的一天，清空当日计数并更新 day_start_utc。
 * 返回 true 表示发生了跨天刷新。不清空累计值，也不惩罚连续天数。
 */
bool couple_state_roll_day(couple_stats_t *stats, uint32_t utc, int32_t tz_offset_minutes);

/* ---------- 计分 ---------- */

/*
 * 记录一次互动并结算能量。
 *
 *   type       couple_event_type_t；MISS / HUG_END / HEART_SYNC / HUG_SYNC 有效
 *   outgoing   true = 本机发出，false = 来自对方
 *   held_ms    HUG_END 的按住时长（毫秒），用来判定抱抱档位；其余事件传 0
 *   utc        事件时间（UTC 秒）
 *   milestone_out  发生里程碑跨越时写入索引，否则写 -1；允许传 NULL
 *
 * 返回本次获得的 Love Energy（可能为 0）。
 */
uint32_t couple_stats_apply_interaction(couple_stats_t *stats, uint8_t type, bool outgoing,
                                        uint16_t held_ms, uint32_t utc,
                                        int32_t tz_offset_minutes, int *milestone_out);

/* ---------- 里程碑 ---------- */

/* 互动总数命中的里程碑索引；没有命中返回 -1。 */
int couple_milestone_for_total(uint32_t total_interactions);

/* 里程碑阈值本身（9 / 18 / 99 / 100 / 365 / 520 / 999 / 1314）。 */
uint32_t couple_milestone_value(int index);

/* 里程碑一次性发放的能量。 */
uint32_t couple_milestone_energy_bonus(int index);

/* 里程碑标题，例如 "1314 · 一生一世"。索引越界返回空串。 */
const char *couple_milestone_title(int index);

/* 该里程碑是否为需要重点庆祝的浪漫彩蛋（520 / 1314）。 */
bool couple_milestone_is_romantic(int index);

/* 标记里程碑已展示；返回 true 表示这是第一次标记。索引越界返回 false。 */
bool couple_milestone_mark_seen(couple_stats_t *stats, int index);

/* ---------- 抱抱 ---------- */

/* 按按住时长（毫秒）判定档位。 */
couple_hug_tier_t couple_hug_tier_for_ms(uint32_t held_ms);

/* 档位中文标签："小抱抱" / "大抱抱" / "不撒手抱抱" / ""。 */
const char *couple_hug_tier_label(couple_hug_tier_t tier);

/* ---------- 文案格式化（纯函数，宿主可测） ---------- */

/* then == 0 或 then > now 时按 0 秒处理。 */
uint32_t couple_seconds_since(uint32_t now_utc, uint32_t then_utc);

/*
 * 相对时长："刚刚" / "3 分钟前" / "2 小时前" / "5 天前"。
 * 返回写入的字节数（不含结尾 NUL）；cap 不足时写空串并返回 0。
 */
size_t couple_format_ago(char *out, size_t cap, uint32_t delta_sec);

/* 本地 24 小时制 "20:07"。返回写入字节数。 */
size_t couple_format_hhmm(char *out, size_t cap, uint32_t utc, int32_t tz_offset_minutes);

/* "你不在的时候，TA想了你3次。"；miss_count == 0 时返回空串。 */
size_t couple_format_offline_summary(char *out, size_t cap, uint16_t miss_count);