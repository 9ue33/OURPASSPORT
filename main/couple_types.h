#pragma once

/*
 * Heartlink 情侣心动终端 —— 跨层共用的枚举与尺寸常量。
 *
 * 本头文件不依赖 ESP-IDF，也不依赖 LVGL，宿主测试可以直接包含。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---------- 标识与容量 ---------- */

#define COUPLE_DEVICE_ID_LEN 16u
#define COUPLE_PAIR_ID_LEN 8u
#define COUPLE_PAIR_TOKEN_LEN 32u
#define COUPLE_NAME_MAX 16u /* UTF-8 字节数上限，中文约 5 个字 */
#define COUPLE_PAIR_CODE_LEN 6u
#define COUPLE_EVENTS_PER_SYNC_MAX 12u
#define COUPLE_EVENT_VALUE_MAX 8u
#define COUPLE_RELAY_URL_MAX 96u
#define COUPLE_WIFI_SSID_MAX 33u
#define COUPLE_WIFI_PASS_MAX 65u

/* ---------- 帧类型（couple_proto.h 的线格式） ---------- */

typedef enum {
    COUPLE_FRAME_PAIR_BEGIN_REQ = 0x01,
    COUPLE_FRAME_PAIR_BEGIN_RSP = 0x02,
    COUPLE_FRAME_PAIR_CLAIM_REQ = 0x03,
    COUPLE_FRAME_PAIR_CLAIM_RSP = 0x04,
    COUPLE_FRAME_PAIR_AWAIT_REQ = 0x05,
    COUPLE_FRAME_PAIR_AWAIT_RSP = 0x06,
    COUPLE_FRAME_SYNC_REQ = 0x07,
    COUPLE_FRAME_SYNC_RSP = 0x08,
    COUPLE_FRAME_UNPAIR_REQ = 0x09,
    COUPLE_FRAME_UNPAIR_RSP = 0x0A,
} couple_frame_type_t;

/* ---------- 响应状态 ---------- */

typedef enum {
    COUPLE_STATUS_OK = 0,
    COUPLE_STATUS_BAD_REQUEST = 1,
    COUPLE_STATUS_CODE_NOT_FOUND = 2,
    COUPLE_STATUS_CODE_EXPIRED = 3,
    COUPLE_STATUS_UNAUTHORIZED = 4,
    COUPLE_STATUS_ALREADY_PAIRED = 5,
    COUPLE_STATUS_SERVER_BUSY = 6,
    COUPLE_STATUS_PAIR_NOT_FOUND = 7,
} couple_status_t;

/* ---------- 互动事件 ---------- */

/*
 * MISS          想你（本机发出）／收到对方的想你
 * HUG_START     长按 OK 开始抱抱
 * HUG_END       松手，value = u16 本次按住时长（毫秒）
 * HUG_SYNC      双方同时在抱 → relay 判定后下发，无 value（len = 0）
 * HEART_SYNC    10 秒内互相想你 → relay 判定后下发
 * UNPAIR        对方解除配对
 * OFFLINE_SUMMARY 上线补拉时的汇总，value[0] = 对方离线期间想你次数
 */
typedef enum {
    COUPLE_EVENT_NONE = 0x00,
    COUPLE_EVENT_MISS = 0x01,
    COUPLE_EVENT_HUG_START = 0x02,
    COUPLE_EVENT_HUG_END = 0x03,
    COUPLE_EVENT_HUG_SYNC = 0x04,
    COUPLE_EVENT_HEART_SYNC = 0x05,
    COUPLE_EVENT_UNPAIR = 0x06,
    COUPLE_EVENT_PAIRED = 0x07,
    COUPLE_EVENT_OFFLINE_SUMMARY = 0x08,
} couple_event_type_t;

/* ---------- 抱抱档位 ---------- */

typedef enum {
    COUPLE_HUG_TIER_NONE = 0,
    COUPLE_HUG_TIER_SMALL = 1, /* 小抱抱 */
    COUPLE_HUG_TIER_BIG = 2,   /* 大抱抱 */
    COUPLE_HUG_TIER_CLINGY = 3 /* 不撒手抱抱 */
} couple_hug_tier_t;

/* ---------- 里程碑 ---------- */

#define COUPLE_MILESTONE_COUNT 8u

typedef enum {
    COUPLE_MILESTONE_9 = 0,
    COUPLE_MILESTONE_18 = 1,
    COUPLE_MILESTONE_99 = 2,
    COUPLE_MILESTONE_100 = 3,
    COUPLE_MILESTONE_365 = 4,
    COUPLE_MILESTONE_520 = 5,
    COUPLE_MILESTONE_999 = 6,
    COUPLE_MILESTONE_1314 = 7,
} couple_milestone_index_t;

/* ---------- 一个事件 ---------- */

typedef struct {
    uint8_t type; /* couple_event_type_t */
    uint8_t len;  /* value 中有效字节数，<= COUPLE_EVENT_VALUE_MAX */
    uint8_t value[COUPLE_EVENT_VALUE_MAX];
} couple_event_t;

typedef struct {
    uint8_t count;
    uint8_t flags; /* bit0 = 本机此刻正在抱抱 */
    couple_event_t items[COUPLE_EVENTS_PER_SYNC_MAX];
} couple_event_list_t;

/* 本机在 SYNC_REQ 里上报的状态位 */
#define COUPLE_SYNC_FLAG_HOLDING 0x01u

/* 配对/绑定进度，用于 SYNC_RSP 的 pair_state */
typedef enum {
    COUPLE_PAIR_STATE_NONE = 0,      /* 尚未配对 */
    COUPLE_PAIR_STATE_PAIRED = 1,    /* 已绑定 */
    COUPLE_PAIR_STATE_PEER_LEFT = 2, /* 对方已解除配对 */
} couple_pair_state_t;