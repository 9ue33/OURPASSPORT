/*
 * couple_state 的 host 测试：不依赖 ESP-IDF/LVGL，只验证纯逻辑与文案。
 *
 * 成功时打印 PASS 并返回 0；任何检查失败都会打印诊断行，最终返回 1。
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "couple_config.h"
#include "couple_state.h"
#include "couple_types.h"

static unsigned g_failed;

static void fail_line(int line, const char *what)
{
    g_failed++;
    printf("FAIL line %d: %s\n", line, what);
}

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            fail_line(__LINE__, #cond);                                   \
        }                                                                 \
    } while (0)

#define CHECK_U(got, want)                                                \
    do {                                                                  \
        unsigned g_ = (unsigned)(got);                                    \
        unsigned w_ = (unsigned)(want);                                   \
        if (g_ != w_) {                                                   \
            g_failed++;                                                   \
            printf("FAIL line %d: got %u want %u\n", __LINE__, g_, w_);   \
        }                                                                 \
    } while (0)

#define CHECK_STR(got, want)                                              \
    do {                                                                  \
        const char *g_ = (got);                                           \
        const char *w_ = (want);                                          \
        if (strcmp(g_, w_) != 0) {                                        \
            g_failed++;                                                   \
            printf("FAIL line %d: got \"%s\" want \"%s\"\n", __LINE__, g_, w_); \
        }                                                                 \
    } while (0)

/* 文案断言：格式化函数有写入副作用，必须先调用再比对内容与返回长度。 */
static void check_text_line(int line, const char *got, unsigned got_len,
                            const char *want)
{
    unsigned want_len = (unsigned)strlen(want);

    if (strcmp(got, want) != 0) {
        g_failed++;
        printf("FAIL line %d: got \"%s\" want \"%s\"\n", line, got, want);
    }
    if (got_len != want_len) {
        g_failed++;
        printf("FAIL line %d: got len %u want %u\n", line, got_len, want_len);
    }
}

#define CHECK_AGO(buf, cap, secs, want)                                   \
    do {                                                                  \
        unsigned n_ = couple_format_ago((buf), (cap), (secs));            \
        check_text_line(__LINE__, (buf), n_, (want));                     \
    } while (0)

#define CHECK_HHMM(buf, cap, utc, tz, want)                               \
    do {                                                                  \
        unsigned n_ = couple_format_hhmm((buf), (cap), (utc), (tz));      \
        check_text_line(__LINE__, (buf), n_, (want));                     \
    } while (0)

#define CHECK_OFFLINE(buf, cap, misses, want)                             \
    do {                                                                  \
        unsigned n_ = couple_format_offline_summary((buf), (cap), (misses)); \
        check_text_line(__LINE__, (buf), n_, (want));                     \
    } while (0)

/* 本地某一天 12:00 对应的 UTC 秒（tz 用 COUPLE_UTC_OFFSET_MINUTES）。 */
static uint32_t local_noon_utc(uint32_t day)
{
    return (uint32_t)((int64_t)day * (int64_t)COUPLE_DAY_SECONDS + 12 * 3600 -
                      (int64_t)COUPLE_UTC_OFFSET_MINUTES * 60);
}

/* ---------- 初始化与校验 ---------- */

static void test_init_and_valid(void)
{
    couple_stats_t zeroed;
    couple_stats_t stats;

    memset(&zeroed, 0, sizeof(zeroed));
    CHECK(!couple_state_valid(&zeroed));
    CHECK(!couple_state_valid(NULL));

    couple_state_init(&stats);
    CHECK(couple_state_valid(&stats));
    CHECK_U(stats.energy, 0u);
    CHECK_U(stats.total_interactions, 0u);
    CHECK_U(stats.streak_days, 0u);
    CHECK_U(stats.last_interaction_day, 0u);
    CHECK_U(stats.day_start_utc, 0u);
    CHECK_U(stats.milestones_seen, 0u);

    stats.schema_version = COUPLE_STATE_SCHEMA_VERSION + 1u;
    CHECK(!couple_state_valid(&stats));

    couple_state_init(&stats);
    stats.magic = COUPLE_STATE_MAGIC - 1u;
    CHECK(!couple_state_valid(&stats));
}

/* ---------- 日界线 ---------- */

typedef struct {
    uint32_t utc;
    int32_t tz;
    uint32_t day;
    uint32_t start;
} day_case_t;

static const day_case_t k_day_cases[] = {
    {0u, 480, 0u, 0u},
    {1759939199u, 480, 20369u, 1759852800u},
    {1759939200u, 480, 20370u, 1759939200u},
    {1759939201u, 480, 20370u, 1759939200u},
    {1728014400u, 480, 20000u, 1727971200u},
    {0u, 0, 0u, 0u},
    {86399u, 0, 0u, 0u},
    {86400u, 0, 1u, 86400u},
    {259200u, 0, 3u, 259200u},
    {0u, -300, 0u, 0u},
    {100u, -300, 0u, 0u},
    {86399u, -300, 0u, 18000u},
    {86400u, -300, 0u, 18000u},
    {1759999999u, -300, 20370u, 1759986000u},
};

static void test_day_boundaries(void)
{
    size_t i;

    for (i = 0u; i < sizeof(k_day_cases) / sizeof(k_day_cases[0]); i++) {
        const day_case_t *c = &k_day_cases[i];
        uint32_t day = couple_day_index(c->utc, c->tz);
        uint32_t start = couple_day_start_utc(c->utc, c->tz);

        CHECK_U(day, c->day);
        CHECK_U(start, c->start);
        CHECK_U(couple_day_index(start, c->tz), day);
        CHECK(start <= c->utc);
    }

    /* 东八区恰好落在本地午夜：日界线就是这一刻，前一秒还属于前一天。 */
    CHECK_U(couple_day_index(1759939199u, 480), 20369u);
    CHECK_U(couple_day_index(1759939200u, 480), 20370u);
    CHECK_U(couple_day_start_utc(1759939200u, 480), 1759939200u);
    CHECK_U(couple_day_start_utc(1759939199u, 480), 1759852800u);

    /* 负时区把本地时间推到 epoch 之前：日序号与日界线都贴到 0。 */
    CHECK_U(couple_day_index(100u, -300), 0u);
    CHECK_U(couple_day_start_utc(100u, -300), 0u);
    CHECK_U(couple_day_start_utc(86399u, -300), 18000u);
    CHECK_U(couple_day_index(18000u, -300), 0u);
}

/* ---------- 跨天刷新 ---------- */

static void test_roll_day(void)
{
    couple_stats_t stats;
    const int32_t tz = COUPLE_UTC_OFFSET_MINUTES;
    uint32_t day0 = local_noon_utc(20000u);
    uint32_t day1 = local_noon_utc(20001u);

    couple_state_init(&stats);
    CHECK(couple_state_roll_day(&stats, day0, tz)); /* 首次调用即建立当天 */
    CHECK_U(stats.day_start_utc, couple_day_start_utc(day0, tz));
    CHECK(!couple_state_roll_day(&stats, day0, tz)); /* 同一天，不刷新 */
    CHECK(!couple_state_roll_day(&stats, day0 + 3600u, tz));

    stats.day_misses = 5u;
    stats.day_hugs = 3u;
    stats.day_first_interaction_utc = day0;
    stats.streak_days = 7u;
    stats.last_interaction_day = 19999u;
    stats.energy = 100u;
    stats.total_interactions = 42u;
    stats.milestones_seen = 0x08u;

    CHECK(!couple_state_roll_day(&stats, day0 + 60u, tz));
    CHECK_U(stats.day_misses, 5u);
    CHECK_U(stats.day_hugs, 3u);
    CHECK_U(stats.day_first_interaction_utc, day0);

    CHECK(couple_state_roll_day(&stats, day1, tz));
    CHECK_U(stats.day_misses, 0u);
    CHECK_U(stats.day_hugs, 0u);
    CHECK_U(stats.day_first_interaction_utc, 0u);
    CHECK_U(stats.streak_days, 7u);
    CHECK_U(stats.last_interaction_day, 19999u);
    CHECK_U(stats.energy, 100u);
    CHECK_U(stats.total_interactions, 42u);
    CHECK_U(stats.milestones_seen, 0x08u);
}

/* ---------- 计分 ---------- */

static void test_scoring(void)
{
    couple_stats_t stats;
    const int32_t tz = COUPLE_UTC_OFFSET_MINUTES;
    const uint32_t utc = 1728014400u; /* 本地第 20000 天的 12:00 */
    int milestone = 123;

    couple_state_init(&stats);

    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc, tz, &milestone), 1u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 1u);
    CHECK_U(stats.misses_sent, 1u);
    CHECK_U(stats.misses_received, 0u);
    CHECK_U(stats.day_misses, 1u);
    CHECK_U(stats.total_interactions, 1u);
    CHECK_U(stats.last_miss_received_utc, 0u);
    CHECK_U(stats.streak_days, 1u);
    CHECK_U(stats.last_interaction_day, 20000u);
    CHECK_U(stats.day_first_interaction_utc, utc);
    CHECK_U(stats.last_unix_time, utc);

    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, false, 0u, utc, tz, &milestone), 1u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 2u);
    CHECK_U(stats.misses_sent, 1u);
    CHECK_U(stats.misses_received, 1u);
    CHECK_U(stats.day_misses, 2u);
    CHECK_U(stats.total_interactions, 2u);
    CHECK_U(stats.last_miss_received_utc, utc);

    /* 心有灵犀与双向抱抱只加能量，不计入互动总数。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HEART_SYNC, false, 0u, utc, tz, &milestone), 5u);
    CHECK_U(stats.energy, 7u);
    CHECK_U(stats.heart_syncs, 1u);
    CHECK_U(stats.total_interactions, 2u);
    CHECK_U(stats.day_misses, 2u);

    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_SYNC, false, 0u, utc, tz, &milestone), 5u);
    CHECK_U(stats.energy, 12u);
    CHECK_U(stats.hugs_shared, 1u);
    CHECK_U(stats.total_interactions, 2u);
    CHECK_U(stats.day_hugs, 0u);

    /*
     * HUG_END：抱抱本身不发能量，但达到「小抱抱」档位就算一次互动，
     * 并进当日抱抱数（双向统计）。档位边界另有 test_hug_scoring 覆盖。
     */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_END, true,
                                           COUPLE_HUG_TIER_SMALL_MS, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 12u);
    CHECK_U(stats.day_hugs, 1u);
    CHECK_U(stats.total_interactions, 3u);

    /* 非计分事件：完全不改变状态。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_START, true, 0u, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_OFFLINE_SUMMARY, false, 0u, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_UNPAIR, false, 0u, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(couple_stats_apply_interaction(&stats, 0xFFu, false, 0u, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 12u);
    CHECK_U(stats.total_interactions, 3u);
    CHECK_U(stats.streak_days, 1u);
    CHECK_U(stats.day_first_interaction_utc, utc);

    /* 过期的 relay 时钟不得把 last_unix_time 改小。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc - 1000u, tz, &milestone), 1u);
    CHECK_U(stats.last_unix_time, utc);
    CHECK_U(stats.energy, 13u);
    CHECK_U(stats.total_interactions, 4u);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, 0u, tz, &milestone), 1u);
    CHECK_U(stats.last_unix_time, utc);
}

/* ---------- 连续天数 ---------- */

static void test_streak(void)
{
    couple_stats_t stats;
    const int32_t tz = COUPLE_UTC_OFFSET_MINUTES;
    uint32_t day20000 = local_noon_utc(20000u);
    uint32_t day20001 = local_noon_utc(20001u);
    uint32_t day20002 = local_noon_utc(20002u);
    uint32_t day20005 = local_noon_utc(20005u);
    uint32_t day20006 = local_noon_utc(20006u);
    int milestone = 123;

    couple_state_init(&stats);

    /* 第一次互动：只把连续天数置为 1，不发奖励。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20000, tz, &milestone), 1u);
    CHECK_U(stats.streak_days, 1u);
    CHECK_U(stats.energy, 1u);

    /* 同一天内重复互动：不再结算连续天数。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20000 + 60u, tz, &milestone), 1u);
    CHECK_U(stats.streak_days, 1u);
    CHECK_U(stats.energy, 2u);

    /* 第二天：续上，+3。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20001, tz, &milestone),
            1u + COUPLE_ENERGY_DAILY_STREAK);
    CHECK_U(stats.streak_days, 2u);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HEART_SYNC, false, 0u, day20001 + 120u, tz, &milestone), 5u);
    CHECK_U(stats.streak_days, 2u);
    CHECK_U(stats.energy, 2u + 4u + 5u);

    /* 第三天：+3。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20002, tz, &milestone),
            1u + COUPLE_ENERGY_DAILY_STREAK);
    CHECK_U(stats.streak_days, 3u);
    CHECK_U(stats.energy, 11u + 4u);

    /* 隔两天再做：连续天数从 1 重新开始，不加奖励、也绝不惩罚。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20005, tz, &milestone), 1u);
    CHECK_U(stats.streak_days, 1u);
    CHECK_U(stats.energy, 16u);
    CHECK_U(stats.total_interactions, 5u);

    /* 紧接着的第二天又续上。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, day20006, tz, &milestone),
            1u + COUPLE_ENERGY_DAILY_STREAK);
    CHECK_U(stats.streak_days, 2u);
    CHECK_U(stats.energy, 20u);
}

/* ---------- 里程碑 ---------- */

static void test_milestones(void)
{
    couple_stats_t stats;
    couple_stats_t fresh;
    const int32_t tz = COUPLE_UTC_OFFSET_MINUTES;
    const uint32_t utc = local_noon_utc(20600u);
    static const uint32_t k_values[COUPLE_MILESTONE_COUNT] = {
        9u, 18u, 99u, 100u, 365u, 520u, 999u, 1314u,
    };
    static const char *const k_titles[COUPLE_MILESTONE_COUNT] = {
        "9 · 长长久久", "18 · 一起吧", "99 · 久久久",  "100 · 百次心动",
        "365 · 一整年", "520 · 我爱你", "999 · 天长地久", "1314 · 一生一世",
    };
    int milestone = 123;
    int index;
    uint32_t i;
    uint32_t gain = 0u;

    /* 查表与文案 */
    for (index = 0; index < (int)COUPLE_MILESTONE_COUNT; index++) {
        CHECK_U(couple_milestone_value(index), k_values[index]);
        CHECK_U(couple_milestone_energy_bonus(index), k_values[index]);
        CHECK_STR(couple_milestone_title(index), k_titles[index]);
        CHECK_U(couple_milestone_for_total(k_values[index]), (unsigned)index);
        /* 阈值 +1 不再命中；99 + 1 == 100 本身是下一个阈值，单独跳过。 */
        if (k_values[index] + 1u != 100u) {
            CHECK(couple_milestone_for_total(k_values[index] + 1u) == -1);
        }
    }
    CHECK_U(couple_milestone_value(-1), 0u);
    CHECK_U(couple_milestone_value((int)COUPLE_MILESTONE_COUNT), 0u);
    CHECK_U(couple_milestone_energy_bonus((int)COUPLE_MILESTONE_COUNT), 0u);
    CHECK_STR(couple_milestone_title(-1), "");
    CHECK_STR(couple_milestone_title((int)COUPLE_MILESTONE_COUNT), "");
    CHECK(couple_milestone_for_total(0u) == -1);
    CHECK(couple_milestone_for_total(1u) == -1);
    CHECK(couple_milestone_for_total(8u) == -1);
    CHECK(couple_milestone_for_total(9u) == 0);
    CHECK(couple_milestone_for_total(10u) == -1);
    CHECK(couple_milestone_for_total(1314u) == 7);
    CHECK(couple_milestone_for_total(1315u) == -1);
    CHECK(couple_milestone_for_total(0xFFFFFFFFu) == -1);

    /* 520 = 索引 5，1314 = 索引 7 才是浪漫彩蛋。 */
    CHECK(couple_milestone_is_romantic(5));
    CHECK(couple_milestone_is_romantic(7));
    CHECK(!couple_milestone_is_romantic(6));
    CHECK(!couple_milestone_is_romantic(4));
    CHECK(!couple_milestone_is_romantic(-1));
    CHECK(!couple_milestone_is_romantic((int)COUPLE_MILESTONE_COUNT));

    /* 恰好第 9 次互动命中索引 0，只发一次。 */
    couple_state_init(&stats);
    for (i = 1u; i <= 9u; i++) {
        gain = couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc, tz, &milestone);
        if (i < 9u) {
            CHECK_U(gain, 1u);
            CHECK(milestone == -1);
        }
    }
    CHECK_U(gain, 1u + 9u);
    CHECK(milestone == 0);
    CHECK_U(stats.total_interactions, 9u);
    CHECK_U(stats.day_misses, 9u);
    CHECK_U(stats.misses_sent, 9u);
    CHECK_U(stats.energy, 18u);
    CHECK_U(stats.milestones_seen, 0x01u);
    CHECK_U(stats.streak_days, 1u);

    /* 第 10 次不再重复触发（精确相等匹配）。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc, tz, &milestone), 1u);
    CHECK(milestone == -1);
    CHECK_U(stats.total_interactions, 10u);
    CHECK_U(stats.energy, 19u);

    CHECK(!couple_milestone_mark_seen(&stats, 0));
    CHECK(!couple_milestone_mark_seen(&stats, (int)COUPLE_MILESTONE_COUNT));
    CHECK(!couple_milestone_mark_seen(&stats, -1));
    CHECK(!couple_milestone_mark_seen(NULL, 0));

    couple_state_init(&fresh);
    CHECK(couple_milestone_mark_seen(&fresh, 3));
    CHECK(!couple_milestone_mark_seen(&fresh, 3));
    CHECK_U(fresh.milestones_seen, 0x08u);

    /* 一路走到 520：命中索引 5 并发放 520 能量。 */
    couple_state_init(&stats);
    for (i = 1u; i <= 520u; i++) {
        gain = couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, false, 0u, utc, tz, &milestone);
        if (i == 100u) {
            CHECK_U(gain, 1u + 100u);
            CHECK(milestone == 3);
        } else if (i == 365u) {
            CHECK_U(gain, 1u + 365u);
            CHECK(milestone == 4);
        } else if (i == 519u) {
            CHECK_U(gain, 1u);
            CHECK(milestone == -1);
        }
    }
    CHECK_U(gain, 1u + 520u);
    CHECK(milestone == 5);
    CHECK_U(stats.total_interactions, 520u);
    CHECK_U(stats.energy, 1631u); /* 520 次基础 + 9/18/99/100/365/520 彩蛋 */
    CHECK_U(stats.milestones_seen, 0x3Fu);
    CHECK_U(stats.misses_received, 520u);
    CHECK_U(stats.last_miss_received_utc, utc);
    CHECK_U(stats.day_misses, 520u);
}

/* ---------- 抱抱档位 ---------- */

static void test_hug_tiers(void)
{
    CHECK(couple_hug_tier_for_ms(0u) == COUPLE_HUG_TIER_NONE);
    CHECK(couple_hug_tier_for_ms(699u) == COUPLE_HUG_TIER_NONE);
    CHECK(couple_hug_tier_for_ms(700u) == COUPLE_HUG_TIER_SMALL);
    CHECK(couple_hug_tier_for_ms(2999u) == COUPLE_HUG_TIER_SMALL);
    CHECK(couple_hug_tier_for_ms(3000u) == COUPLE_HUG_TIER_BIG);
    CHECK(couple_hug_tier_for_ms(7999u) == COUPLE_HUG_TIER_BIG);
    CHECK(couple_hug_tier_for_ms(8000u) == COUPLE_HUG_TIER_CLINGY);
    CHECK(couple_hug_tier_for_ms(60000u) == COUPLE_HUG_TIER_CLINGY);
    CHECK(couple_hug_tier_for_ms(UINT32_MAX) == COUPLE_HUG_TIER_CLINGY);

    CHECK_STR(couple_hug_tier_label(COUPLE_HUG_TIER_NONE), "");
    CHECK_STR(couple_hug_tier_label(COUPLE_HUG_TIER_SMALL), "小抱抱");
    CHECK_STR(couple_hug_tier_label(COUPLE_HUG_TIER_BIG), "大抱抱");
    CHECK_STR(couple_hug_tier_label(COUPLE_HUG_TIER_CLINGY), "不撒手抱抱");
    CHECK_STR(couple_hug_tier_label((couple_hug_tier_t)99), "");
}

/* ---------- 抱抱计分 ---------- */

static void test_hug_scoring(void)
{
    couple_stats_t stats;
    const int32_t tz = COUPLE_UTC_OFFSET_MINUTES;
    const uint32_t utc = local_noon_utc(20000u);
    int milestone = 123;

    /*
     * 没到「小抱抱」档位的误触：不算一次互动，不进当日抱抱数，
     * 也不许占掉「今日首次互动」和连续天数——否则手抖会把连续天数带偏。
     */
    couple_state_init(&stats);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_END, true, 699u, utc, tz, &milestone), 0u);
    CHECK_U(stats.energy, 0u);
    CHECK_U(stats.total_interactions, 0u);
    CHECK_U(stats.day_hugs, 0u);
    CHECK_U(stats.day_first_interaction_utc, 0u);
    CHECK_U(stats.streak_days, 0u);
    CHECK_U(stats.last_interaction_day, 0u);

    /* 正好 700ms = 小抱抱：算一次互动，但抱抱本身不发能量。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_END, true,
                                           COUPLE_HUG_TIER_SMALL_MS, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 0u);
    CHECK_U(stats.total_interactions, 1u);
    CHECK_U(stats.day_hugs, 1u);
    CHECK_U(stats.day_first_interaction_utc, utc);
    CHECK_U(stats.streak_days, 1u);

    /* 大抱抱：按得久不会多算，仍然只是一次互动。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_END, false,
                                           COUPLE_HUG_TIER_BIG_MS, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 0u);
    CHECK_U(stats.total_interactions, 2u);
    CHECK_U(stats.day_hugs, 2u);

    /* 不撒手抱抱：同理，只记一次。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_END, false,
                                           COUPLE_HUG_TIER_CLINGY_MS, utc, tz, &milestone), 0u);
    CHECK(milestone == -1);
    CHECK_U(stats.energy, 0u);
    CHECK_U(stats.total_interactions, 3u);
    CHECK_U(stats.day_hugs, 3u);

    /* 双向抱抱的能量只由 HUG_SYNC 发一次，HUG_END 不重复发放。 */
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_HUG_SYNC, false, 0u, utc, tz, &milestone), 5u);
    CHECK_U(stats.energy, 5u);
    CHECK_U(stats.hugs_shared, 1u);
    CHECK_U(stats.total_interactions, 3u);
    CHECK_U(stats.day_hugs, 3u);
}

/* ---------- 文案 ---------- */

static void test_seconds_since(void)
{
    CHECK_U(couple_seconds_since(100u, 0u), 0u);
    CHECK_U(couple_seconds_since(100u, 100u), 0u);
    CHECK_U(couple_seconds_since(100u, 200u), 0u);
    CHECK_U(couple_seconds_since(100u, 40u), 60u);
    CHECK_U(couple_seconds_since(0u, 0u), 0u);
}

static void test_format_ago(void)
{
    char buf[32];

    CHECK_AGO(buf, sizeof(buf), 0u, "刚刚");
    CHECK_AGO(buf, sizeof(buf), 59u, "刚刚");
    CHECK_AGO(buf, sizeof(buf), 60u, "1 分钟前");
    CHECK_AGO(buf, sizeof(buf), 3599u, "59 分钟前");
    CHECK_AGO(buf, sizeof(buf), 3600u, "1 小时前");
    CHECK_AGO(buf, sizeof(buf), 86399u, "23 小时前");
    CHECK_AGO(buf, sizeof(buf), 86400u, "1 天前");
    CHECK_AGO(buf, sizeof(buf), 400000u, "4 天前");
    CHECK_AGO(buf, sizeof(buf), 0xFFFFFFFFu, "49710 天前");
}

static void test_format_hhmm(void)
{
    char buf[32];

    CHECK_HHMM(buf, sizeof(buf), 1759939199u, 480, "23:59");
    /* 本地跨过日界线：同一秒之后就是新一天的 00:00。 */
    CHECK_HHMM(buf, sizeof(buf), 1759939200u, 480, "00:00");
    CHECK_HHMM(buf, sizeof(buf), 1728014400u, 480, "12:00");
    CHECK_HHMM(buf, sizeof(buf), 100u, 480, "08:01");
    CHECK_HHMM(buf, sizeof(buf), 86399u, 0, "23:59");
    CHECK_HHMM(buf, sizeof(buf), 86400u, 0, "00:00");
    CHECK_HHMM(buf, sizeof(buf), 262861u, 0, "01:01");
    /* UTC-5：本地时间落在前一天的 19:01。 */
    CHECK_HHMM(buf, sizeof(buf), 100u, -300, "19:01");
}

static void test_format_offline_summary(void)
{
    char buf[64];

    CHECK_OFFLINE(buf, sizeof(buf), 0u, "");
    CHECK_OFFLINE(buf, sizeof(buf), 1u, "你不在的时候，TA想了你1次。");
    CHECK_OFFLINE(buf, sizeof(buf), 3u, "你不在的时候，TA想了你3次。");
    CHECK_OFFLINE(buf, sizeof(buf), 12u, "你不在的时候，TA想了你12次。");
    CHECK_OFFLINE(buf, sizeof(buf), 65535u, "你不在的时候，TA想了你65535次。");
}

/* ---------- 容量护栏 ---------- */

static void check_cap_bounds(size_t cap)
{
    char buf[16];
    size_t i;

    memset(buf, '#', sizeof(buf));
    CHECK_U(couple_format_ago(buf, cap, 3600u), 0u);
    CHECK_U(couple_format_hhmm(buf, cap, 1728014400u, 480), 0u);
    CHECK_U(couple_format_offline_summary(buf, cap, 3u), 0u);
    if (cap > 0u) {
        CHECK(buf[0] == '\0'); /* 容量不足时写空串 */
    }
    for (i = cap; i < sizeof(buf); i++) {
        CHECK(buf[i] == '#'); /* 越过 cap 的哨兵字节不得被改写 */
    }
}

static void test_capacity_guards(void)
{
    char buf[16];
    const char *want = "1 小时前";

    check_cap_bounds(0u);
    check_cap_bounds(1u);
    check_cap_bounds(5u);

    /* 刚好装下（长度 + 结尾 NUL）可以成功，少一个字节就失败。 */
    memset(buf, '#', sizeof(buf));
    CHECK_U(couple_format_ago(buf, strlen(want) + 1u, 3600u), strlen(want));
    CHECK_STR(buf, want);
    CHECK(buf[strlen(want) + 1u] == '#');

    memset(buf, '#', sizeof(buf));
    CHECK_U(couple_format_ago(buf, strlen(want), 3600u), 0u);
    CHECK_STR(buf, "");

    memset(buf, '#', sizeof(buf));
    CHECK_U(couple_format_hhmm(buf, 6u, 1728014400u, 480), 5u);
    CHECK_STR(buf, "12:00");
    CHECK(buf[6] == '#');

    memset(buf, '#', sizeof(buf));
    CHECK_U(couple_format_hhmm(buf, 5u, 1728014400u, 480), 0u);
    CHECK_STR(buf, "");

    CHECK_U(couple_format_offline_summary(NULL, 0u, 3u), 0u);
    CHECK_U(couple_format_ago(NULL, 0u, 3600u), 0u);
    CHECK_U(couple_format_hhmm(NULL, 0u, 0u, 0), 0u);
}

/* ---------- 空指针护栏 ---------- */

static void test_null_milestone_out(void)
{
    couple_stats_t stats;
    const uint32_t utc = local_noon_utc(20000u);

    couple_state_init(&stats);
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc,
                                           COUPLE_UTC_OFFSET_MINUTES, NULL), 1u);
    CHECK_U(stats.total_interactions, 1u);
    CHECK_U(stats.energy, 1u);

    /* 不带 milestone_out 也能正常触发彩蛋，只是调用方拿不到索引。 */
    stats.total_interactions = 8u;
    stats.milestones_seen = 0u;
    CHECK_U(couple_stats_apply_interaction(&stats, COUPLE_EVENT_MISS, true, 0u, utc,
                                           COUPLE_UTC_OFFSET_MINUTES, NULL), 1u + 9u);
    CHECK_U(stats.milestones_seen, 0x01u);

    CHECK(!couple_state_roll_day(NULL, utc, COUPLE_UTC_OFFSET_MINUTES));
    couple_state_init(NULL);
    CHECK_U(couple_stats_apply_interaction(NULL, COUPLE_EVENT_MISS, true, 0u, utc, 0, NULL), 0u);
}

int main(void)
{
    test_init_and_valid();
    test_day_boundaries();
    test_roll_day();
    test_scoring();
    test_streak();
    test_milestones();
    test_hug_tiers();
    test_hug_scoring();
    test_seconds_since();
    test_format_ago();
    test_format_hhmm();
    test_format_offline_summary();
    test_capacity_guards();
    test_null_milestone_out();

    if (g_failed != 0u) {
        printf("test_couple_state: FAIL (%u checks failed)\n", g_failed);
        return 1;
    }
    printf("test_couple_state: PASS\n");
    return 0;
}