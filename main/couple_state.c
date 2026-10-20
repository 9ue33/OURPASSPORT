/*
 * Heartlink 情侣心动终端 —— 关系状态、Love Energy、每日系统与本地文案格式化。
 *
 * 纯 C 实现，不依赖 ESP-IDF，也不依赖 LVGL，可以在宿主机上直接编译和测试。
 * 计分规则、日界语义与文案以 couple_state.h 的头部注释为唯一权威定义。
 * 设计红线：不施加压力，断联或隔天都不产生责备性文案或惩罚。
 */

#include "couple_state.h"

#include <stdio.h>
#include <string.h>

#include "couple_config.h"

/* ---------- 内部常量与工具 ---------- */

#define COUPLE_SECONDS_PER_DAY ((int64_t)COUPLE_DAY_SECONDS)

static const uint32_t k_milestone_value[COUPLE_MILESTONE_COUNT] = {
    9u, 18u, 99u, 100u, 365u, 520u, 999u, 1314u,
};

static const char *const k_milestone_title[COUPLE_MILESTONE_COUNT] = {
    "9 · 长长久久", "18 · 一起吧", "99 · 久久久",  "100 · 百次心动",
    "365 · 一整年", "520 · 我爱你", "999 · 天长地久", "1314 · 一生一世",
};

/* 向下取整的整数除法（divisor 恒为正）。 */
static int64_t floor_div(int64_t value, int64_t divisor)
{
    int64_t quotient = value / divisor;
    if (value < 0 && value % divisor != 0) {
        quotient--;
    }
    return quotient;
}

/* UTC 秒 + 时区偏移 = 本地秒（可能是负数）。 */
static int64_t local_seconds(uint32_t utc, int32_t tz_offset_minutes)
{
    return (int64_t)utc + (int64_t)tz_offset_minutes * 60;
}

/* 会结算能量的四类互动；其余事件不参与每日互动与连续天数。 */
static bool type_is_scored(uint8_t type)
{
    return type == COUPLE_EVENT_MISS || type == COUPLE_EVENT_HUG_END ||
           type == COUPLE_EVENT_HEART_SYNC || type == COUPLE_EVENT_HUG_SYNC;
}

/* 写入 src；cap 装不下（含结尾 NUL）时写空串并返回 0。 */
static size_t copy_text(char *out, size_t cap, const char *src)
{
    size_t len = strlen(src);

    if (out == NULL || cap <= len) {
        if (out != NULL && cap > 0u) {
            out[0] = '\0';
        }
        return 0u;
    }
    memcpy(out, src, len + 1u);
    return len;
}

/* ---------- 状态初始化与校验 ---------- */

void couple_state_init(couple_stats_t *stats)
{
    if (stats == NULL) {
        return;
    }
    memset(stats, 0, sizeof(*stats));
    stats->magic = COUPLE_STATE_MAGIC;
    stats->schema_version = COUPLE_STATE_SCHEMA_VERSION;
}

bool couple_state_valid(const couple_stats_t *stats)
{
    if (stats == NULL) {
        return false;
    }
    return stats->magic == COUPLE_STATE_MAGIC &&
           stats->schema_version == COUPLE_STATE_SCHEMA_VERSION;
}

/* ---------- 日界线 ---------- */

uint32_t couple_day_index(uint32_t utc, int32_t tz_offset_minutes)
{
    int64_t day = floor_div(local_seconds(utc, tz_offset_minutes), COUPLE_SECONDS_PER_DAY);

    if (day < 0) {
        day = 0; /* 本地时间落在 1970 之前，统一按第 0 天处理 */
    }
    return (uint32_t)day;
}

uint32_t couple_day_start_utc(uint32_t utc, int32_t tz_offset_minutes)
{
    int64_t day = floor_div(local_seconds(utc, tz_offset_minutes), COUPLE_SECONDS_PER_DAY);
    int64_t start = day * COUPLE_SECONDS_PER_DAY - (int64_t)tz_offset_minutes * 60;

    if (start < 0) {
        start = 0; /* 日界线跨到 epoch 之前时贴到 0，保证 start <= utc */
    }
    return (uint32_t)start;
}

bool couple_state_roll_day(couple_stats_t *stats, uint32_t utc, int32_t tz_offset_minutes)
{
    uint32_t start;

    if (stats == NULL) {
        return false;
    }
    start = couple_day_start_utc(utc, tz_offset_minutes);
    if (start == stats->day_start_utc) {
        return false;
    }
    stats->day_start_utc = start;
    stats->day_misses = 0u;
    stats->day_hugs = 0u;
    stats->day_first_interaction_utc = 0u;
    return true;
}

/* ---------- 计分 ---------- */

uint32_t couple_stats_apply_interaction(couple_stats_t *stats, uint8_t type, bool outgoing,
                                        uint16_t held_ms, uint32_t utc,
                                        int32_t tz_offset_minutes, int *milestone_out)
{
    uint32_t gain = 0u;
    int milestone;

    if (milestone_out != NULL) {
        *milestone_out = -1;
    }
    if (stats == NULL) {
        return 0u;
    }

    (void)couple_state_roll_day(stats, utc, tz_offset_minutes);
    if (!type_is_scored(type)) {
        return 0u; /* 非计分事件不改变任何状态 */
    }
    if (type == COUPLE_EVENT_HUG_END && couple_hug_tier_for_ms(held_ms) == COUPLE_HUG_TIER_NONE) {
        /*
         * 按太短的误触（没到小抱抱档位）：不算一次互动，
         * 也不让它占掉「今日首次互动」，否则连续天数会被误触带偏。
         */
        return 0u;
    }

    if (stats->day_first_interaction_utc == 0u) {
        uint32_t today;

        stats->day_first_interaction_utc = utc;
        today = couple_day_index(utc, tz_offset_minutes);
        if (stats->streak_days > 0u && (uint32_t)stats->last_interaction_day + 1u == today) {
            stats->streak_days++;
            gain += (uint32_t)COUPLE_ENERGY_DAILY_STREAK;
        } else if (stats->streak_days > 0u && (uint32_t)stats->last_interaction_day == today) {
            /* 同一天里重复互动：连续天数已经结算过，不重复发放 */
        } else {
            /* 首次互动或隔了几天：重新从 1 起算，不扣能量、不产生任何惩罚 */
            stats->streak_days = 1u;
        }
        stats->last_interaction_day = (uint16_t)today;
    }

    switch (type) {
    case COUPLE_EVENT_MISS:
        if (outgoing) {
            gain += (uint32_t)COUPLE_ENERGY_MISS_SENT;
            stats->misses_sent++;
        } else {
            gain += (uint32_t)COUPLE_ENERGY_MISS_RECEIVED;
            stats->misses_received++;
            stats->last_miss_received_utc = utc;
        }
        stats->day_misses++;
        stats->total_interactions++;
        break;

    case COUPLE_EVENT_HUG_END:
        /*
         * 走到这里的抱抱必然已经达到小抱抱档位（NONE 在前面就返回了）。
         * 抱抱本身不发能量：能量体现在双向抱抱 HUG_SYNC 上。
         */
        stats->day_hugs++;
        stats->total_interactions++;
        break;

    case COUPLE_EVENT_HEART_SYNC:
        gain += (uint32_t)COUPLE_ENERGY_HEART_SYNC;
        stats->heart_syncs++;
        break;

    case COUPLE_EVENT_HUG_SYNC:
        gain += (uint32_t)COUPLE_ENERGY_HUG_SYNC;
        stats->hugs_shared++;
        break;

    default:
        break;
    }

    if (utc > 0u && utc > stats->last_unix_time) {
        stats->last_unix_time = utc; /* 单调护栏，避免 relay 回拨把时间改小 */
    }

    milestone = couple_milestone_for_total(stats->total_interactions);
    if (milestone >= 0 && couple_milestone_mark_seen(stats, milestone)) {
        gain += couple_milestone_energy_bonus(milestone);
        if (milestone_out != NULL) {
            *milestone_out = milestone;
        }
    }
    stats->energy += gain;
    return gain;
}

/* ---------- 里程碑 ---------- */

int couple_milestone_for_total(uint32_t total_interactions)
{
    int index;

    for (index = 0; index < (int)COUPLE_MILESTONE_COUNT; index++) {
        if (k_milestone_value[index] == total_interactions) {
            return index;
        }
    }
    return -1;
}

uint32_t couple_milestone_value(int index)
{
    if (index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        return 0u;
    }
    return k_milestone_value[index];
}

uint32_t couple_milestone_energy_bonus(int index)
{
    return couple_milestone_value(index); /* 阈值本身即是彩蛋能量 */
}

const char *couple_milestone_title(int index)
{
    if (index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        return "";
    }
    return k_milestone_title[index];
}

bool couple_milestone_is_romantic(int index)
{
    uint32_t value = couple_milestone_value(index);

    return value == 520u || value == 1314u;
}

bool couple_milestone_mark_seen(couple_stats_t *stats, int index)
{
    uint32_t bit;

    if (stats == NULL || index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        return false;
    }
    bit = 1u << (unsigned)index;
    if ((stats->milestones_seen & bit) != 0u) {
        return false;
    }
    stats->milestones_seen |= bit;
    return true;
}

/* ---------- 抱抱 ---------- */

couple_hug_tier_t couple_hug_tier_for_ms(uint32_t held_ms)
{
    if (held_ms >= (uint32_t)COUPLE_HUG_TIER_CLINGY_MS) {
        return COUPLE_HUG_TIER_CLINGY;
    }
    if (held_ms >= (uint32_t)COUPLE_HUG_TIER_BIG_MS) {
        return COUPLE_HUG_TIER_BIG;
    }
    if (held_ms >= (uint32_t)COUPLE_HUG_TIER_SMALL_MS) {
        return COUPLE_HUG_TIER_SMALL;
    }
    return COUPLE_HUG_TIER_NONE;
}

const char *couple_hug_tier_label(couple_hug_tier_t tier)
{
    switch (tier) {
    case COUPLE_HUG_TIER_SMALL:
        return "小抱抱";
    case COUPLE_HUG_TIER_BIG:
        return "大抱抱";
    case COUPLE_HUG_TIER_CLINGY:
        return "不撒手抱抱";
    case COUPLE_HUG_TIER_NONE:
    default:
        return "";
    }
}

/* ---------- 文案格式化 ---------- */

uint32_t couple_seconds_since(uint32_t now_utc, uint32_t then_utc)
{
    if (then_utc == 0u || then_utc >= now_utc) {
        return 0u;
    }
    return now_utc - then_utc;
}

size_t couple_format_ago(char *out, size_t cap, uint32_t delta_sec)
{
    char buf[24];

    if (delta_sec < (uint32_t)COUPLE_RELATIVE_JUST_NOW_SEC) {
        return copy_text(out, cap, "刚刚");
    }
    if (delta_sec < 3600u) {
        (void)snprintf(buf, sizeof(buf), "%u 分钟前", (unsigned)(delta_sec / 60u));
    } else if (delta_sec < 86400u) {
        (void)snprintf(buf, sizeof(buf), "%u 小时前", (unsigned)(delta_sec / 3600u));
    } else {
        (void)snprintf(buf, sizeof(buf), "%u 天前", (unsigned)(delta_sec / 86400u));
    }
    return copy_text(out, cap, buf);
}

size_t couple_format_hhmm(char *out, size_t cap, uint32_t utc, int32_t tz_offset_minutes)
{
    char buf[8];
    int64_t local = local_seconds(utc, tz_offset_minutes) % COUPLE_SECONDS_PER_DAY;

    if (local < 0) {
        local += COUPLE_SECONDS_PER_DAY;
    }
    (void)snprintf(buf, sizeof(buf), "%02u:%02u", (unsigned)(local / 3600),
                   (unsigned)((local % 3600) / 60));
    return copy_text(out, cap, buf);
}

size_t couple_format_offline_summary(char *out, size_t cap, uint16_t miss_count)
{
    char buf[48];

    if (miss_count == 0u) {
        return copy_text(out, cap, "");
    }
    (void)snprintf(buf, sizeof(buf), "你不在的时候，TA想了你%u次。", (unsigned)miss_count);
    return copy_text(out, cap, buf);
}