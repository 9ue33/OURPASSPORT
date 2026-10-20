/*
 * Heartlink 情侣心动终端 —— 应用编排层实现。
 *
 * 契约与线程模型见 couple_app.h。这里只强调三条最容易写错的：
 *
 *   1. 只有 app 任务碰慢资源。按键回调、couple_net 回调、esp_timer 回调
 *      一律只做值拷贝 + 非阻塞入队；队列满就丢（丢按键不会坏状态）。
 *   2. 所有「现在几点了」的判断都用 now_ms()/ms_pending()/ms_elapsed()，
 *      绝不裸比较毫秒——esp_timer 的 32 位毫秒会在 49.7 天回绕。
 *   3. 结算（加 Love Energy / 记里程碑）只有一个入口 score_interaction()，
 *      且未对时（utc == 0）时整段跳过：couple_stats_apply_interaction() 开头
 *      会按 utc 滚日界，拿 0 去滚会把今天的计数清掉。
 */

#include "couple_app.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "bsp_battery.h"
#include "bsp_button.h"

#include "couple_audio.h"
#include "couple_config.h"
#include "couple_net.h"
#include "couple_proto.h"
#include "couple_prov.h"
#include "couple_radio.h"
#include "couple_state.h"
#include "couple_store.h"
#include "couple_store_blob.h"
#include "couple_tone.h"
#include "couple_types.h"
#include "couple_ui.h"
#include "couple_ui_strings.h"
#include "couple_wifi.h"

static const char *TAG = "couple_app";

/* ---------- 本模块自己的节奏参数 ---------- */

#define APP_QUEUE_DEPTH 16
#define APP_TASK_STACK 4096
#define APP_TASK_PRIO 4
#define APP_TASK_NAME "couple_app"

#define APP_QUIT_WAIT_MS 1000   /* shutdown 等 app 任务退出的上限 */
#define APP_QUIESCE_MS 2000     /* shutdown 等音频静音的上限 */

#define APP_BATTERY_INTERVAL_MS 5000 /* 电量计读得慢，别跟 1 Hz 一起刷 */

/* 内存长线打点周期。LVGL 用的是自己的静态池，系统堆是另一块；调大池子等于直接从
 * 系统堆里挖走，所以两边的余量都要能看到。「整机不响应」时这行日志是唯一的指针。 */
#define APP_MEM_LOG_INTERVAL_MS 10000

/* 各类「轻确认页」自己停留多久（0 = sticky，只能被按键退回） */
#define APP_MISS_SENT_MS 1200
#define APP_MISS_RECV_MS 4000
#define APP_HUG_RECV_MS 4000
#define APP_HUG_SYNC_MS 8000
#define APP_HEART_SYNC_MS 5000
#define APP_OFFLINE_MS 5000
#define APP_PAIR_DONE_MS 2500
#define APP_PROV_WAIT_MS 20000
#define APP_PROV_DONE_MS 2500
#define APP_MILESTONE_MS 6000
#define APP_MILESTONE_ROMANTIC_MS 8000 /* 520 / 1314 值得多看两秒 */

/*
 * OK 键唯一的长短判定：按下满这么久就进抱抱，不足则算一次「想你」/「确认」。
 * 不再用按键库的 CLICK / LONG 判长短——CLICK 要等松手后约 185ms 才上报，
 * LONG 又卡在 500ms 的硬边界上；「按住想抱抱，对方却收到想你」就是这两条叠出来的。
 */
#define APP_HUG_ARM_MS 400

/*
 * 解绑是唯一不可逆的操作，屏幕写的是「按住 OK 确认解除」。跟抱抱共用 400ms
 * 太轻了：手抖一下、或者只是想反悔的那次短按，都可能被算成确认，配对删掉
 * 之后两边都要重新生成配对码。单独放到 1.5 秒，长到足以让人意识到自己在
 * 做什么。见 PRESS 分支：解绑页武装这个时长，其他页面仍是 APP_HUG_ARM_MS。
 */
#define APP_UNPAIR_HOLD_MS 1500
#define APP_FLAVOR_CHANCE_PERCENT 40 /* 彩蛋文案的出现概率 */

/* 闲置多久关背光。对方互动会点亮，按键也会点亮。 */
#define APP_SLEEP_IDLE_MS 30000
/* 唤醒后这段时间内到达的 CLICK 一律吞掉：它是「按一下点亮屏幕」那一次按压
 * 松手约 185ms 后补发的，会把 UP/DOWN 变成菜单翻页或配对数字。
 * 只拦 CLICK：PRESS/RELEASE 是 OK 键抱抱/想你的唯一真源，吞掉会让抱抱关不掉。 */
#define APP_WAKE_GUARD_MS 900

/*
 * SoftAP 配置页地址。couple_wifi_ap_start() 没有暴露自定义 IP 的接口，
 * 这里按 ESP-IDF SoftAP 的默认网段写死（未在真机上验证过，交付说明里列为待验证）。
 */
#define APP_PROV_AP_IP "192.168.4.1"

/* ---------- 队列消息 ---------- */

typedef enum {
    APP_MSG_KEY = 1,
    APP_MSG_TICK,        /* 1 Hz */
    APP_MSG_HOLD_TICK,   /* 抱抱期间 20 Hz */
    APP_MSG_OK_ARM,      /* OK 按住满 APP_HUG_ARM_MS（由 s_press_timer 投递） */
    APP_MSG_NET_STATE,
    APP_MSG_NET_SYNC,
    APP_MSG_NET_PAIR,
    APP_MSG_PROV_SAVED,
    APP_MSG_PROV_STATUS,
    APP_MSG_QUIT,
} app_msg_kind_t;

typedef struct {
    uint8_t kind;
    union {
        struct {
            uint8_t btn;
            uint8_t ev;
        } key;
        uint8_t net_state;
        struct {
            bool partner_online;
            bool partner_holding;
            uint8_t pair_state;
            couple_event_list_t events;
        } sync;
        couple_net_pair_event_t pair;
        struct {
            char ssid[COUPLE_WIFI_SSID_MAX];
            char pass[COUPLE_WIFI_PASS_MAX];
            char relay[COUPLE_RELAY_URL_MAX];
            char name[COUPLE_NAME_MAX + 1u]; /* 空串 = 保持当前名字 */
        } prov_saved;
        bool prov_client;
    } u;
} app_msg_t;

/* 队列元素是要 memcpy 的，别让它长到吃任务栈。 */
_Static_assert(sizeof(app_msg_t) <= 256, "app_msg_t too large");

/* ---------- 流程状态 ---------- */

typedef enum {
    APP_FLOW_NONE = 0,
    APP_FLOW_MENU,        /* 三项菜单 */
    APP_FLOW_PAIR_CHOICE, /* 配对：申请 / 录入 二选一 */
    APP_FLOW_PAIR_HOST,   /* 配对：本机出码，等对方录入 */
    APP_FLOW_PAIR_JOIN,   /* 配对：录入对方的码 */
    APP_FLOW_PAIR_DONE,   /* 配对成功定格 */
    APP_FLOW_PROV,        /* Wi-Fi 配网 */
    APP_FLOW_UNPAIR,      /* 解绑确认 */
} app_flow_t;

typedef enum {
    APP_PROV_IDLE = 0,
    APP_PROV_AP,     /* 热点与配置页已起，等手机保存 */
    APP_PROV_WAIT_IP,/* 已保存，等 STA 拿到 IP */
    APP_PROV_DONE,
    APP_PROV_FAILED,
} app_prov_state_t;

/* ---------- 静态状态 ---------- */

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_task_exited;
static volatile bool s_shutting_down;
static esp_timer_handle_t s_tick_timer;
static esp_timer_handle_t s_hold_timer;
static esp_timer_handle_t s_press_timer; /* OK 按住满 APP_HUG_ARM_MS 时投一条 APP_MSG_OK_ARM */
static bool s_started;

static app_flow_t s_flow;
static uint8_t s_menu_sel;
static char s_partner_name[COUPLE_NAME_MAX + 1u];

static bool s_hug_active;
static uint32_t s_hug_start_ms;
static couple_hug_tier_t s_hug_tier;
static bool s_hug_synced;
static bool s_loop_owned; /* 循环心跳是本机起的，收尾时也只由本机停 */

static uint32_t s_wake_guard_until_ms; /* 只拦唤醒后补发的 CLICK */
static uint32_t s_ok_press_ms;         /* OK 按下时刻；0 = 当前没按着 */
/* 最近一次「有人在」的时刻：按键与对方发来的互动都会刷新它。 */
static uint32_t s_last_input_ms;

static bool s_transient_active;
static uint32_t s_transient_until_ms; /* 0 = sticky */

static uint32_t s_pair_deadline_ms;
static char s_pair_code[COUPLE_PAIR_CODE_LEN + 1u];
static uint8_t s_pair_digits[COUPLE_PAIR_CODE_LEN];
static uint8_t s_pair_filled;
static uint32_t s_pair_done_ms;

static app_prov_state_t s_prov_state;
static uint8_t s_prov_clients;
static bool s_prov_ap_drawn;
static uint32_t s_prov_deadline_ms;

static int s_battery_percent; /* -1 = 读不到 */
static uint32_t s_battery_ms;
static uint32_t s_mem_ms; /* 上次打印内存打点的时刻，见 APP_MEM_LOG_INTERVAL_MS */

static bool s_partner_online;
static bool s_peer_holding;
static bool s_burst_notice_shown;
static uint32_t s_dropped_keys;

static couple_net_hooks_t s_net_hooks;

/*
 * 前置声明集中放在这里：这些函数互相调用且定义顺序靠后，
 * 不声明就会被当成隐式声明（-Werror 必挂）。
 */
static void clear_transient(void);
static void arm_transient(uint32_t ms);
static void resume_sta_for_app(void);
static void enter_prov(void);
static void refresh_prov_ap(void);
static void on_prov_saved(const char *ssid, const char *pass, const char *relay_url,
                          const char *name, void *user);
static void on_prov_status(bool client_connected, void *user);

/* ---------- 时间助手 ---------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* deadline 还在未来？deadline == 0 视为「没设」。 */
static bool ms_pending(uint32_t now, uint32_t deadline)
{
    return deadline != 0u && (int32_t)(deadline - now) > 0;
}

/* deadline 已过？deadline == 0 视为「没设」。 */
static bool ms_elapsed(uint32_t now, uint32_t deadline)
{
    return deadline != 0u && (int32_t)(now - deadline) >= 0;
}

/*
 * 定长字符串拷贝。couple_store_copy() 已经处理了 NULL 与空 cap，
 * 这里只是给它一个短名字，让下面满屏的 snprintf/copy 好读一点。
 */
static void copy_str(char *dst, size_t cap, const char *src)
{
    (void)couple_store_copy(dst, cap, src);
}

/* ---------- 入队 ---------- */

/*
 * 非阻塞入队。按键丢了不坏状态，所以满了只计数、不等待。
 * 所有回调上下文（按键、net、esp_timer、httpd）都只走这里。
 */
static bool post(const app_msg_t *msg)
{
    if (s_queue == NULL || s_shutting_down) {
        return false;
    }
    if (xQueueSend(s_queue, msg, 0) != pdTRUE) {
        s_dropped_keys++;
        return false;
    }
    return true;
}

/* ---------- 计分 ---------- */

typedef struct {
    uint8_t type;
    bool outgoing;
    uint16_t held_ms;
    uint32_t utc;
    int milestone; /* 出参：-1 = 本次没踩到里程碑 */
} score_args_t;

/*
 * 跑在 couple_store 的锁里，必须极短：只做纯计算，不碰 LVGL/网络/音频/日志。
 */
static void score_cb(couple_stats_t *stats, void *user)
{
    score_args_t *args = (score_args_t *)user;

    (void)couple_stats_apply_interaction(stats, args->type, args->outgoing, args->held_ms,
                                         args->utc, COUPLE_UTC_OFFSET_MINUTES, &args->milestone);
}

/*
 * 唯一结算入口。返回本次踩到的里程碑下标，-1 表示没有。
 * 未对时（utc == 0）时整段跳过——见文件头第 3 条。
 */
static int score_interaction(uint8_t type, bool outgoing, uint16_t held_ms)
{
    const uint32_t utc = couple_net_clock_now_utc();
    if (utc == 0u) {
        return -1;
    }

    score_args_t args;
    memset(&args, 0, sizeof(args));
    args.type = type;
    args.outgoing = outgoing;
    args.held_ms = held_ms;
    args.utc = utc;
    args.milestone = -1;

    couple_store_stats_update(score_cb, &args);
    return args.milestone;
}

/* ---------- 里程碑 ---------- */

static void milestone_tone(int index)
{
    if (index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        return;
    }
    (void)couple_audio_play(couple_milestone_is_romantic(index) ? COUPLE_TONE_MILESTONE_BIG
                                                               : COUPLE_TONE_MILESTONE);
}

/* 正在抱抱（或被 sticky 页占屏）时只出横幅，不抢页面。 */
static void milestone_toast(int index)
{
    if (index < 0 || index >= (int)COUPLE_MILESTONE_COUNT) {
        return;
    }
    couple_ui_toast(couple_milestone_title(index));
}

static void show_milestone_if_any(int index)
{
    if (index < 0) {
        return;
    }
    if (index >= (int)COUPLE_MILESTONE_COUNT) {
        couple_ui_toast(COUPLE_STR_MILESTONE_OOR_TOAST);
        return;
    }
    if (s_hug_active) {
        milestone_toast(index);
        return;
    }

    couple_ui_show_milestone(index);
    milestone_tone(index);
    arm_transient(couple_milestone_is_romantic(index) ? APP_MILESTONE_ROMANTIC_MS : APP_MILESTONE_MS);
}

/* ---------- transient（会自己回首页的临时页） ---------- */

static void clear_transient(void)
{
    s_transient_active = false;
    s_transient_until_ms = 0u;
}

/* ms == 0 表示 sticky：不自回首页，只能被按键退回。 */
static void arm_transient(uint32_t ms)
{
    s_transient_active = true;
    s_transient_until_ms = (ms == 0u) ? 0u : (now_ms() + ms);
}

/* ---------- 页面流转 ---------- */

static void refresh_status(void);

static void return_home(void)
{
    couple_ui_show(COUPLE_VIEW_HOME);
    refresh_status();
}

/*
 * 从任意子流程退回首页。这是唯一一个「清干净全部流程状态」的地方。
 * 注意顺序：所有页面切换都发生在 toast 之前——toast 是顶层悬浮对象，
 * 但先切页再提示能避开「切页把横幅一起刷掉」的风险。
 */
static void escape_to_home(void)
{
    const app_flow_t previous = s_flow;

    s_flow = APP_FLOW_NONE;
    clear_transient();
    s_pair_deadline_ms = 0u;
    s_pair_done_ms = 0u;

    if (previous == APP_FLOW_PAIR_CHOICE || previous == APP_FLOW_PAIR_HOST ||
        previous == APP_FLOW_PAIR_JOIN || previous == APP_FLOW_PAIR_DONE) {
        couple_net_pair_cancel();
        s_pair_code[0] = '\0';
        s_pair_filled = 0u;
    }

    if (previous == APP_FLOW_PROV) {
        couple_prov_stop();
        resume_sta_for_app();
        (void)couple_net_settings_changed();
        s_prov_state = APP_PROV_IDLE;
        s_prov_deadline_ms = 0u;
        s_prov_ap_drawn = false;
    }

    return_home();

    if (previous == APP_FLOW_PROV) {
        couple_ui_toast(COUPLE_STR_PROV_EXIT_TOAST);
    }
}

/* ---------- 互动：想你 ---------- */

static void send_miss(void)
{
    if (!couple_store_is_paired()) {
        couple_ui_toast(COUPLE_STR_NEED_PAIR_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        return;
    }

    /* MISS 是零长度事件：协议里 len 必须为 0，别用 couple_event_set_u16()。 */
    couple_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)COUPLE_EVENT_MISS;
    ev.len = 0u;

    if (couple_net_enqueue_event(&ev) != ESP_OK) {
        couple_ui_toast(COUPLE_STR_PAIR_ERR_GENERIC);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        refresh_status();
        return;
    }

    ESP_LOGI(TAG, "按键: 发想你");

    if (couple_net_is_online()) {
        couple_ui_show_miss_sent();
    } else {
        /* outbox 收下了，但首页要如实说「一上线就送到」。 */
        couple_ui_show(COUPLE_VIEW_MISS_SENT);
        couple_ui_toast(COUPLE_STR_MISS_QUEUED_SUB);
    }
    (void)couple_audio_play(COUPLE_TONE_MISS_SEND);
    arm_transient(APP_MISS_SENT_MS);

    show_milestone_if_any(score_interaction((uint8_t)COUPLE_EVENT_MISS, true, 0u));
    refresh_status();
}

/* ---------- 互动：抱抱 ---------- */

static bool start_hug(uint32_t press_ms)
{
    if (!couple_store_is_paired()) {
        couple_ui_toast(COUPLE_STR_NEED_PAIR_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        return false;
    }
    if (s_hug_active) {
        return true;
    }

    couple_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = (uint8_t)COUPLE_EVENT_HUG_START;
    ev.len = 0u;
    (void)couple_net_enqueue_event(&ev);
    couple_net_set_holding(true);

    s_hug_active = true;
    /* 从「按下去」那一刻起算，不是从武装成功那一刻：否则 400ms 会白送给档位。 */
    s_hug_start_ms = press_ms;
    s_hug_tier = COUPLE_HUG_TIER_NONE;
    s_hug_synced = false;

    clear_transient();
    (void)couple_audio_play(COUPLE_TONE_HUG_START);
    couple_ui_show_hug_hold(0u, COUPLE_HUG_TIER_NONE);
    (void)esp_timer_start_periodic(s_hold_timer, (uint64_t)COUPLE_HUG_HOLD_TICK_MS * 1000u);

    ESP_LOGI(TAG, "按键: 发抱抱 START (press=%ums)", (unsigned)press_ms);
    return true;
}

/* 20 Hz：把「按住多久了」翻译成档位与进度环。 */
static void on_hold_tick(void)
{
    if (!s_hug_active) {
        return;
    }

    uint32_t held = now_ms() - s_hug_start_ms;
    if (held > COUPLE_HUG_MAX_MS) {
        held = COUPLE_HUG_MAX_MS;
    }

    const couple_hug_tier_t tier = couple_hug_tier_for_ms(held);
    if (tier != s_hug_tier) {
        s_hug_tier = tier;
        if (tier == COUPLE_HUG_TIER_CLINGY) {
            (void)couple_audio_play(COUPLE_TONE_HUG_SYNC);
        } else if (tier != COUPLE_HUG_TIER_NONE) {
            (void)couple_audio_play(COUPLE_TONE_HEARTBEAT);
        }
    }

    /* 已经进了 HUG CONNECTED 就别再拿单方面的进度环盖掉同步动画。 */
    if (!s_hug_synced) {
        couple_ui_show_hug_hold(held, s_hug_tier);
    }
}

static void end_hug(void)
{
    if (!s_hug_active) {
        return;
    }

    uint32_t held = now_ms() - s_hug_start_ms;
    if (held > COUPLE_HUG_MAX_MS) {
        held = COUPLE_HUG_MAX_MS;
    }
    const couple_hug_tier_t tier = s_hug_tier;

    (void)esp_timer_stop(s_hold_timer);
    s_hug_active = false;
    couple_net_set_holding(false);

    couple_event_t ev;
    memset(&ev, 0, sizeof(ev));
    couple_event_set_u16(&ev, (uint8_t)COUPLE_EVENT_HUG_END, (uint16_t)held);
    (void)couple_net_enqueue_event(&ev);

    if (s_loop_owned) {
        couple_audio_stop_loop();
        s_loop_owned = false;
    }
    s_hug_synced = false;
    s_hug_tier = COUPLE_HUG_TIER_NONE;

    (void)couple_audio_play(COUPLE_TONE_HUG_END);

    /* 太短的一次按压不算「抱过」，别污染互动总数与档位统计。 */
    const int milestone =
        (tier != COUPLE_HUG_TIER_NONE) ? score_interaction((uint8_t)COUPLE_EVENT_HUG_END, true,
                                                           (uint16_t)held)
                                       : -1;

    if (s_peer_holding) {
        /* 对方还抱着 → 停在「TA抱住了你」，让这一刻留着。 */
        couple_ui_show_hug_received(0u);
        arm_transient(0u);
        milestone_toast(milestone);
    } else {
        clear_transient();
        return_home();
        show_milestone_if_any(milestone);
    }

    ESP_LOGI(TAG, "按键: 发抱抱 END (held=%ums tier=%d)", (unsigned)held, (int)tier);
}

/* ---------- HUG CONNECTED ---------- */

static void enter_hug_sync(void)
{
    if (s_hug_synced) {
        return;
    }
    s_hug_synced = true;
    clear_transient();
    couple_ui_show_hug_sync();
    (void)couple_audio_play(COUPLE_TONE_HUG_SYNC);
    if (couple_audio_loop(COUPLE_TONE_HEARTBEAT)) {
        s_loop_owned = true;
    }
}

static void leave_hug_sync(void)
{
    if (!s_hug_synced) {
        return;
    }
    s_hug_synced = false;

    if (s_loop_owned) {
        couple_audio_stop_loop();
        s_loop_owned = false;
    }

    if (s_hug_active) {
        uint32_t held = now_ms() - s_hug_start_ms;
        if (held > COUPLE_HUG_MAX_MS) {
            held = COUPLE_HUG_MAX_MS;
        }
        couple_ui_show_hug_hold(held, s_hug_tier);
    } else {
        return_home();
    }
}

/* ---------- 菜单与子流程入口 ---------- */

static void enter_menu(void)
{
    s_flow = APP_FLOW_MENU;
    s_menu_sel = 0u;
    clear_transient();
    couple_ui_show(COUPLE_VIEW_MENU);
    couple_ui_show_menu(0u);
}

static void enter_daily(void)
{
    clear_transient();
    couple_ui_show(COUPLE_VIEW_DAILY);
    arm_transient(0u);
}

static void enter_pair(void)
{
    s_flow = APP_FLOW_PAIR_CHOICE;
    clear_transient();
    couple_ui_show(COUPLE_VIEW_PAIR);
    couple_ui_show_pair_status(COUPLE_STR_PAIR_HEADLINE, COUPLE_STR_PAIR_CHOICE_DETAIL, false, true);
}

static void start_pair_host(void)
{
    if (couple_net_pair_begin() != ESP_OK) {
        couple_ui_show_pair_status(COUPLE_STR_PAIR_ERR_NETWORK, COUPLE_STR_PAIR_ERR_GENERIC, false,
                                   false);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        return;
    }
    s_flow = APP_FLOW_PAIR_HOST;
    couple_ui_show_pair_status(COUPLE_STR_PAIR_APPLY_HEADLINE, COUPLE_STR_PAIR_APPLY_DETAIL, true,
                               true);
}

static void enter_pair_join(void)
{
    s_flow = APP_FLOW_PAIR_JOIN;
    s_pair_filled = 0u;
    memset(s_pair_digits, 0, sizeof(s_pair_digits));
    couple_ui_show_pair_entry(s_pair_digits, s_pair_filled, 0u);
}

/* UP/DOWN：改光标所在这一位。光标位置就是「已确认位数」，填满后钉在最后一位。 */
static void pair_entry_adjust(int direction)
{
    const uint8_t idx = (s_pair_filled < COUPLE_PAIR_CODE_LEN)
                            ? s_pair_filled
                            : (uint8_t)(COUPLE_PAIR_CODE_LEN - 1u);
    int value = (int)s_pair_digits[idx] + direction;
    while (value < 0) {
        value += 10;
    }
    s_pair_digits[idx] = (uint8_t)(value % 10);
    couple_ui_show_pair_entry(s_pair_digits, s_pair_filled, idx);
}

static void pair_commit(void);

/* OK：确认这一位；第 6 位之后再按一次就提交。 */
static void pair_entry_confirm(void)
{
    if (s_pair_filled < COUPLE_PAIR_CODE_LEN) {
        s_pair_filled++;
        const uint8_t cursor =
            (s_pair_filled >= COUPLE_PAIR_CODE_LEN) ? (uint8_t)(COUPLE_PAIR_CODE_LEN - 1u)
                                                    : s_pair_filled;
        couple_ui_show_pair_entry(s_pair_digits, s_pair_filled, cursor);
        return;
    }
    pair_commit();
}

static void pair_commit(void)
{
    /* 协议要求 6 个字节都不为 NUL，且不必以 NUL 结尾。 */
    char code[COUPLE_PAIR_CODE_LEN];
    for (uint8_t i = 0u; i < COUPLE_PAIR_CODE_LEN; i++) {
        code[i] = (char)('0' + s_pair_digits[i]);
    }

    couple_ui_show_pair_status(COUPLE_STR_PAIR_SUBMIT_HEADLINE, COUPLE_STR_PAIR_SUBMIT_DETAIL, true,
                               true);

    if (couple_net_pair_claim(code) != ESP_OK) {
        couple_ui_show_pair_status(COUPLE_STR_PAIR_ERR_NETWORK, COUPLE_STR_PAIR_ERR_GENERIC, false,
                                   false);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
    }
}

static const char *pair_error_headline(uint8_t status)
{
    switch (status) {
    case COUPLE_STATUS_CODE_NOT_FOUND:
    case COUPLE_STATUS_PAIR_NOT_FOUND:
        return COUPLE_STR_PAIR_ERR_NOT_FOUND;
    case COUPLE_STATUS_CODE_EXPIRED:
        return COUPLE_STR_PAIR_ERR_EXPIRED;
    case COUPLE_STATUS_UNAUTHORIZED:
        return COUPLE_STR_PAIR_ERR_WRONG;
    case COUPLE_STATUS_ALREADY_PAIRED:
        return COUPLE_STR_PAIR_ERR_ALREADY;
    case COUPLE_STATUS_SERVER_BUSY:
        return COUPLE_STR_PAIR_ERR_BUSY;
    case COUPLE_NET_STATUS_NETWORK:
    case COUPLE_NET_STATUS_LOCAL:
        return COUPLE_STR_PAIR_ERR_NETWORK;
    default:
        return COUPLE_STR_PAIR_ERR_GENERIC;
    }
}

/* 失败：录入阶段保留已输入的数字，其它阶段退回二选一页面。 */
static void pair_fail(const char *headline)
{
    if (s_flow == APP_FLOW_PAIR_JOIN) {
        couple_ui_show_pair_entry(s_pair_digits, s_pair_filled,
                                  (s_pair_filled == 0u) ? 0u : (uint8_t)(s_pair_filled - 1u));
        couple_ui_toast(headline);
    } else {
        s_flow = APP_FLOW_PAIR_CHOICE;
        s_pair_deadline_ms = 0u;
        s_pair_code[0] = '\0';
        couple_ui_show_pair_status(headline, COUPLE_STR_PAIR_ERR_GENERIC, false, false);
    }
    (void)couple_audio_play(COUPLE_TONE_ERROR);
}

static void pair_success(const char *name)
{
    s_flow = APP_FLOW_PAIR_DONE;
    s_pair_deadline_ms = 0u;
    s_pair_code[0] = '\0';

    /*
     * 三元组由 couple_net 自己写进 store（它有 token 与 partner_id），
     * 这里只负责把它落盘，绝不重复调 couple_store_set_pair()。
     */
    (void)couple_store_flush();

    if (name != NULL && name[0] != '\0') {
        copy_str(s_partner_name, sizeof(s_partner_name), name);
    } else {
        couple_store_blob_t blob;
        couple_store_snapshot(&blob);
        copy_str(s_partner_name, sizeof(s_partner_name), blob.partner_name);
    }

    if (!couple_store_is_paired()) {
        ESP_LOGW(TAG, "配对回调到了，但 store 里没有配对关系");
    }

    couple_ui_show_pair_confirmed(s_partner_name);
    (void)couple_audio_play(COUPLE_TONE_MILESTONE);
    clear_transient();
    s_pair_done_ms = now_ms() + APP_PAIR_DONE_MS;
    refresh_status();
}

static void handle_pair(const couple_net_pair_event_t *ev)
{
    switch (ev->phase) {
    case COUPLE_NET_PAIR_BEGIN:
        s_flow = APP_FLOW_PAIR_HOST;
        couple_ui_show_pair_status(COUPLE_STR_PAIR_APPLY_HEADLINE, COUPLE_STR_PAIR_APPLY_DETAIL,
                                   true, true);
        break;

    case COUPLE_NET_PAIR_AWAITING:
        s_flow = APP_FLOW_PAIR_HOST;
        copy_str(s_pair_code, sizeof(s_pair_code), ev->code);
        s_pair_deadline_ms = (ev->ttl_secs == 0u) ? 0u : (now_ms() + (uint32_t)ev->ttl_secs * 1000u);
        couple_ui_show_pair_offer(s_pair_code, ev->ttl_secs);
        break;

    case COUPLE_NET_PAIR_PAIRED:
    case COUPLE_NET_PAIR_CLAIMED:
        pair_success(ev->partner_name);
        break;

    case COUPLE_NET_PAIR_EXPIRED:
        pair_fail(COUPLE_STR_PAIR_ERR_EXPIRED);
        break;

    case COUPLE_NET_PAIR_FAILED:
        pair_fail(pair_error_headline(ev->status));
        break;

    case COUPLE_NET_PAIR_UNPAIRED:
        s_flow = APP_FLOW_NONE;
        s_pair_deadline_ms = 0u;
        s_pair_done_ms = 0u;
        s_pair_code[0] = '\0';
        s_partner_name[0] = '\0';
        clear_transient();
        return_home();
        couple_ui_toast(COUPLE_STR_UNPAIRED_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        refresh_status();
        break;

    case COUPLE_NET_PAIR_NONE:
    default:
        break;
    }
}

/* ---------- 解绑 ---------- */

static void prompt_unpair(void)
{
    if (!couple_store_is_paired()) {
        couple_ui_toast(COUPLE_STR_NEED_PAIR_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        return;
    }

    s_flow = APP_FLOW_UNPAIR;
    clear_transient();
    couple_ui_show(COUPLE_VIEW_PAIR);
    couple_ui_show_unpair_prompt(s_partner_name);
}

static void do_unpair(void)
{
    couple_audio_cancel();

    /*
     * 先清本地、再通知 relay：用户的意图优先，网络不好也不许两台设备
     * 继续显示成一对。
     */
    (void)couple_store_clear_pair();
    (void)couple_store_flush();
    (void)couple_net_unpair();

    s_flow = APP_FLOW_NONE;
    s_pair_code[0] = '\0';
    s_pair_deadline_ms = 0u;
    s_pair_done_ms = 0u;
    clear_transient();
    return_home();
    couple_ui_toast(COUPLE_STR_UNPAIRED_TOAST);
    refresh_status();
}

static void activate_menu_item(uint8_t index)
{
    switch (index) {
    case 0u:
        enter_pair();
        break;
    case 1u:
        enter_prov();
        break;
    case 2u:
        prompt_unpair();
        break;
    default:
        break;
    }
}

/* ---------- 配网 ---------- */

static void enter_prov(void)
{
    s_flow = APP_FLOW_PROV;
    s_prov_state = APP_PROV_AP;
    s_prov_clients = 0u;
    s_prov_deadline_ms = 0u;
    s_prov_ap_drawn = false;
    clear_transient();

    couple_ui_show(COUPLE_VIEW_PROV);
    couple_ui_show_prov_status(COUPLE_STR_PROV_STARTING_HEADLINE, COUPLE_STR_PROV_STARTING_DETAIL,
                               true, true);

    /* AP 与 STA 不同时运行。 */
    couple_wifi_sta_stop();

    couple_prov_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.on_saved = on_prov_saved;
    hooks.on_status = on_prov_status;
    hooks.user = NULL;

    couple_store_blob_t blob;
    couple_store_snapshot(&blob);
    const char *relay = (blob.relay_url[0] != '\0') ? blob.relay_url : NULL;

    /* hooks 按值拷贝，可以放在栈上。blob 也在栈上，名字只在 start 里被拷贝。 */
    if (couple_prov_start(&hooks, relay, blob.self_name) != ESP_OK) {
        ESP_LOGE(TAG, "配网启动失败");
        s_flow = APP_FLOW_NONE;
        s_prov_state = APP_PROV_IDLE;
        s_prov_ap_drawn = false;
        return_home();
        couple_ui_toast(COUPLE_STR_PROV_START_FAIL_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        resume_sta_for_app();
        return;
    }

    couple_ui_show_prov_ap(couple_prov_ap_ssid(), COUPLE_PROV_AP_PASS, APP_PROV_AP_IP, 0u);
    s_prov_ap_drawn = true;
}

/* 手机接入/断开由 1 Hz 轮询得到，这里只在真变化时重绘。 */
static void refresh_prov_ap(void)
{
    if (s_flow != APP_FLOW_PROV || s_prov_state != APP_PROV_AP) {
        return;
    }
    const uint8_t clients = couple_wifi_ap_client_count();
    if (clients == s_prov_clients && s_prov_ap_drawn) {
        return;
    }
    s_prov_clients = clients;
    couple_ui_show_prov_ap(couple_prov_ap_ssid(), COUPLE_PROV_AP_PASS, APP_PROV_AP_IP, clients);
    s_prov_ap_drawn = true;
}

static void resume_sta_for_app(void)
{
    couple_store_blob_t blob;
    couple_store_snapshot(&blob);
    if (blob.wifi_ssid[0] == '\0') {
        return;
    }
    (void)couple_wifi_sta_start(blob.wifi_ssid, blob.wifi_pass);
}

static void finish_prov(bool ok)
{
    s_prov_state = ok ? APP_PROV_DONE : APP_PROV_FAILED;
    s_prov_deadline_ms = 0u;
    if (ok) {
        couple_ui_show_prov_status(COUPLE_STR_PROV_DONE_HEADLINE, COUPLE_STR_PROV_DONE_DETAIL, false,
                                   true);
        (void)couple_audio_play(COUPLE_TONE_MILESTONE);
    } else {
        couple_ui_show_prov_status(COUPLE_STR_PROV_FAIL_HEADLINE, COUPLE_STR_PROV_FAIL_DETAIL, false,
                                   false);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
    }
    arm_transient(APP_PROV_DONE_MS);
}

static void handle_prov_tick(uint32_t now)
{
    switch (s_prov_state) {
    case APP_PROV_AP:
        refresh_prov_ap();
        break;

    case APP_PROV_WAIT_IP:
        if (couple_wifi_sta_has_ip()) {
            finish_prov(true);
        } else if (ms_elapsed(now, s_prov_deadline_ms)) {
            finish_prov(false);
        }
        break;

    case APP_PROV_IDLE:
    case APP_PROV_DONE:
    case APP_PROV_FAILED:
    default:
        break;
    }
}

/* httpd 任务（栈 6144）：只允许拷贝 + 入队。绝不调 stop/start，绝不打印密码。 */
static void on_prov_saved(const char *ssid, const char *pass, const char *relay_url,
                          const char *name, void *user)
{
    (void)user;

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_PROV_SAVED;
    copy_str(msg.u.prov_saved.ssid, sizeof(msg.u.prov_saved.ssid), ssid);
    copy_str(msg.u.prov_saved.pass, sizeof(msg.u.prov_saved.pass), pass);
    copy_str(msg.u.prov_saved.relay, sizeof(msg.u.prov_saved.relay), relay_url);
    copy_str(msg.u.prov_saved.name, sizeof(msg.u.prov_saved.name), name);
    (void)post(&msg);
}

/* esp_timer 任务：只入队。 */
static void on_prov_status(bool client_connected, void *user)
{
    (void)user;

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_PROV_STATUS;
    msg.u.prov_client = client_connected;
    (void)post(&msg);
}

static void handle_prov_saved(const app_msg_t *msg)
{
    const char *ssid = msg->u.prov_saved.ssid;
    const char *pass = msg->u.prov_saved.pass;
    const char *relay = msg->u.prov_saved.relay;
    const char *name = msg->u.prov_saved.name;

    if (ssid[0] == '\0') {
        return;
    }

    ESP_LOGI(TAG, "配网已保存: ssid=%s relay=%s", ssid, relay);
    (void)couple_store_set_wifi(ssid, pass);
    if (relay[0] != '\0') {
        (void)couple_store_set_relay_url(relay);
    }
    /* 名字留空表示「保持当前名字」，所以只在有值时才改；清洗交给 store 自己做。 */
    if (name[0] != '\0') {
        (void)couple_store_set_names(name, NULL);
    }
    (void)couple_store_flush();

    couple_ui_show_prov_status(COUPLE_STR_PROV_SAVED_HEADLINE, COUPLE_STR_PROV_CONNECTING, true,
                               true);
    couple_prov_stop();
    resume_sta_for_app();
    (void)couple_net_settings_changed();

    s_prov_state = APP_PROV_WAIT_IP;
    s_prov_deadline_ms = now_ms() + APP_PROV_WAIT_MS;
}

/* ---------- 彩蛋文案 ---------- */

static const char *pick_flavor(void)
{
    static const char *const eggs[8] = {
        COUPLE_STR_EGG_1, COUPLE_STR_EGG_2, COUPLE_STR_EGG_3, COUPLE_STR_EGG_4,
        COUPLE_STR_EGG_5, COUPLE_STR_EGG_6, COUPLE_STR_EGG_7, COUPLE_STR_EGG_8,
    };
    static char burst[64];

    couple_stats_t stats;
    couple_store_stats_read(&stats);

    /*
     * 「今天第 N 次想你」按当日双向计数恰好跨过阈值触发，一天只响一次。
     * 它优先于随机彩蛋——这条文案是今天唯一一次，不能被 40% 的概率吃掉。
     */
    if (!s_burst_notice_shown && stats.day_misses >= (uint16_t)COUPLE_MISS_BURST_NOTICE) {
        s_burst_notice_shown = true;
        (void)snprintf(burst, sizeof(burst), COUPLE_STR_EGG_BURST_FMT, (unsigned)stats.day_misses);
        return burst;
    }

    if ((esp_random() % 100u) < (uint32_t)APP_FLAVOR_CHANCE_PERCENT) {
        return eggs[esp_random() % 8u];
    }
    return NULL;
}

/* ---------- 同步（收到对方的东西） ---------- */

static void handle_sync(const app_msg_t *msg)
{
    const uint32_t now = now_ms();

    s_partner_online = msg->u.sync.partner_online;

    const couple_event_list_t *list = &msg->u.sync.events;
    uint8_t count = list->count;
    if (count > (uint8_t)COUPLE_EVENTS_PER_SYNC_MAX) {
        count = (uint8_t)COUPLE_EVENTS_PER_SYNC_MAX;
    }

    bool got_miss = false;
    bool got_heart = false;
    bool got_hug_sync = false;
    bool got_hug_start = false;
    bool got_hug_end = false;
    bool got_offline = false;
    bool got_unpair = false;
    uint16_t offline_count = 0u;
    uint16_t hug_ms = 0u;
    int milestone = -1;

    /* 第一趟：先把分数结清，再决定画什么。 */
    for (uint8_t i = 0u; i < count; i++) {
        const couple_event_t *ev = &list->items[i];
        int got = -1;

        switch ((couple_event_type_t)ev->type) {
        case COUPLE_EVENT_MISS:
            got_miss = true;
            got = score_interaction((uint8_t)COUPLE_EVENT_MISS, false, 0u);
            break;

        case COUPLE_EVENT_HUG_START:
            got_hug_start = true;
            break;

        case COUPLE_EVENT_HUG_END: {
            got_hug_end = true;
            uint16_t held = 0u;
            if (couple_event_get_u16(ev, &held) && held > hug_ms) {
                hug_ms = held;
            }
            got = score_interaction((uint8_t)COUPLE_EVENT_HUG_END, false, hug_ms);
            break;
        }

        case COUPLE_EVENT_HUG_SYNC:
            got_hug_sync = true;
            got = score_interaction((uint8_t)COUPLE_EVENT_HUG_SYNC, false, 0u);
            break;

        case COUPLE_EVENT_HEART_SYNC:
            got_heart = true;
            got = score_interaction((uint8_t)COUPLE_EVENT_HEART_SYNC, false, 0u);
            break;

        case COUPLE_EVENT_OFFLINE_SUMMARY:
            got_offline = true;
            if (ev->len >= 1u) {
                offline_count = (uint16_t)ev->value[0];
            }
            break;

        case COUPLE_EVENT_UNPAIR:
            got_unpair = true;
            break;

        default:
            break;
        }

        if (got >= 0) {
            milestone = got;
        }
    }

    ESP_LOGI(TAG, "收到: miss=%d hug_start=%d hug_end=%d hug_sync=%d heart=%d offline=%d unpair=%d",
             (int)got_miss, (int)got_hug_start, (int)got_hug_end, (int)got_hug_sync, (int)got_heart,
             (int)got_offline, (int)got_unpair);

    bool displayed = false;

    /* 第二趟：按优先级展示，先命中的赢。 */
    if (got_unpair || msg->u.sync.pair_state == (uint8_t)COUPLE_PAIR_STATE_PEER_LEFT) {
        const bool was_paired = couple_store_is_paired();

        if (was_paired) {
            (void)couple_store_clear_pair();
        }
        s_partner_name[0] = '\0';

        /* 对方走了，本机可能还按着 OK：先把抱抱收干净再换页。 */
        if (s_hug_active) {
            (void)esp_timer_stop(s_hold_timer);
            s_hug_active = false;
            s_hug_tier = COUPLE_HUG_TIER_NONE;
            couple_net_set_holding(false);
        }
        if (s_loop_owned) {
            couple_audio_stop_loop();
            s_loop_owned = false;
        }
        s_hug_synced = false;

        s_flow = APP_FLOW_NONE;
        clear_transient();
        return_home();
        couple_ui_toast(COUPLE_STR_PEER_LEFT_TOAST);
        (void)couple_audio_play(COUPLE_TONE_ERROR);
        ESP_LOGW(TAG, "对方已解除绑定");
        /* 刚才那一屏是对方引起的：别让闲置计时器在下一拍就把屏幕熄掉。 */
        s_last_input_ms = now;
        return;
    }

    if (got_offline && offline_count > 0u) {
        /*
         * 离线汇总优先于同批的逐条 MISS：relay 会一边发汇总一边把每条补回来，
         * 让逐条动画盖掉汇总就丢了这次互动最重的那份情绪。
         */
        couple_ui_show_offline_summary(offline_count);
        (void)couple_audio_play(COUPLE_TONE_MISS_RECV);
        arm_transient(APP_OFFLINE_MS);
        displayed = true;
    } else if (got_heart) {
        if (s_hug_active) {
            /* 抱抱页每 50 ms 就会重绘，全屏 HEART SYNC 实际看不见：只出声提示。 */
            (void)couple_audio_play(COUPLE_TONE_HEART_SYNC);
            couple_ui_toast(COUPLE_STR_EGG_5);
        } else {
            couple_ui_show_heart_sync();
            (void)couple_audio_play(COUPLE_TONE_HEART_SYNC);
            arm_transient(APP_HEART_SYNC_MS);
        }
        displayed = true;
    } else if (got_hug_sync) {
        if (s_hug_active) {
            enter_hug_sync();
        } else {
            couple_ui_show_hug_sync();
            (void)couple_audio_play(COUPLE_TONE_HUG_SYNC);
            arm_transient(APP_HUG_SYNC_MS);
        }
        displayed = true;
    } else if (got_miss) {
        const char *flavor = pick_flavor();
        const char *text = (flavor != NULL) ? flavor : COUPLE_STR_MISS_RECV_FLAVOR_DEFAULT;
        if (s_hug_active) {
            couple_ui_toast(text);
        } else {
            couple_ui_show_miss_received(text);
            arm_transient(APP_MISS_RECV_MS);
        }
        (void)couple_audio_play(COUPLE_TONE_MISS_RECV);
        displayed = true;
    } else if (got_hug_end) {
        if (s_hug_active) {
            (void)couple_audio_play(COUPLE_TONE_HUG_END);
        } else {
            couple_ui_show_hug_received(hug_ms);
            (void)couple_audio_play(COUPLE_TONE_HUG_END);
            arm_transient(APP_HUG_RECV_MS);
        }
        displayed = true;
    } else if (got_hug_start) {
        if (s_hug_active) {
            (void)couple_audio_play(COUPLE_TONE_HUG_START);
        } else {
            couple_ui_show_hug_received(0u);
            (void)couple_audio_play(COUPLE_TONE_HUG_START);
            /* 对方还按着就停在这一页，撒手了才自己收。 */
            arm_transient(msg->u.sync.partner_holding ? 0u : APP_HUG_RECV_MS);
        }
        displayed = true;
    }

    /* 520 / 1314 绝对不能被别的动画吃掉；普通的只在没抢到屏时补一次。 */
    if (milestone >= 0 && (couple_milestone_is_romantic(milestone) || !displayed)) {
        show_milestone_if_any(milestone);
    }

    /* 实时「对方正按住 OK」的变化沿：这是双向抱抱的唯一依据。 */
    if (msg->u.sync.partner_holding != s_peer_holding) {
        const bool holding = msg->u.sync.partner_holding;
        s_peer_holding = holding;
        s_last_input_ms = now;

        if (holding) {
            if (s_hug_active && !s_hug_synced) {
                enter_hug_sync();
            } else if (!s_hug_active && couple_ui_view() != COUPLE_VIEW_HUG_RECV) {
                couple_ui_show_hug_received(0u);
                arm_transient(0u);
            }
        } else {
            if (s_hug_synced) {
                leave_hug_sync();
            } else if (!s_hug_active && s_transient_active && s_transient_until_ms == 0u &&
                       couple_ui_view() == COUPLE_VIEW_HUG_RECV) {
                arm_transient(APP_HUG_RECV_MS);
            }
        }
    }

    /*
     * 对方发来的互动在屏幕上出现了：把闲置计时器重置。
     * 否则「睡着时收到想你」会先被 show_view() 点亮，紧接的 1 Hz tick 发现
     * 闲置已经超过 30 秒，又立刻熄掉——消息只闪了不到一秒。
     */
    if (displayed) {
        s_last_input_ms = now;
    }

    refresh_status();
}

/* ---------- 常驻状态推送 ---------- */

static void refresh_status(void)
{
    couple_store_blob_t blob;
    couple_store_snapshot(&blob);

    couple_stats_t stats;
    couple_store_stats_read(&stats);

    couple_ui_status_t st;
    memset(&st, 0, sizeof(st));
    st.paired = couple_store_is_paired();
    st.partner_known = (blob.partner_name[0] != '\0');
    st.partner_online = s_partner_online;
    st.wifi_connected = couple_wifi_sta_has_ip();
    st.battery_percent = (int8_t)s_battery_percent;
    st.wifi_rssi = (int8_t)couple_wifi_sta_rssi();
    st.now_utc = couple_net_clock_now_utc();
    st.energy = stats.energy;
    st.total_interactions = stats.total_interactions;
    st.day_misses = stats.day_misses;
    st.day_hugs = stats.day_hugs;
    st.streak_days = stats.streak_days;
    st.day_first_interaction_utc = stats.day_first_interaction_utc;
    st.last_miss_received_utc = stats.last_miss_received_utc;
    copy_str(st.partner_name, sizeof(st.partner_name), blob.partner_name);

    copy_str(s_partner_name, sizeof(s_partner_name), blob.partner_name);

    couple_ui_push_status(&st);
}

/* ---------- 按键 ---------- */

static void ok_click(void)
{
    switch (s_flow) {
    case APP_FLOW_MENU:
        activate_menu_item(s_menu_sel);
        return;
    case APP_FLOW_PAIR_CHOICE:
        start_pair_host();
        return;
    case APP_FLOW_PAIR_HOST:
        couple_ui_toast(COUPLE_STR_PAIR_JOIN_WAIT_DETAIL);
        return;
    case APP_FLOW_PAIR_JOIN:
        pair_entry_confirm();
        return;
    case APP_FLOW_PAIR_DONE:
        s_pair_done_ms = 0u;
        escape_to_home();
        return;
    case APP_FLOW_UNPAIR:
        /* 解绑是破坏性操作，屏幕上的提示是「按住 OK 确认解除」：
           确认只在长按时发生（见 handle_ok_arm），短按只是反悔退出。 */
        escape_to_home();
        return;
    case APP_FLOW_PROV:
        return; /* 配网期间按键不参与：在手机上完成 */
    case APP_FLOW_NONE:
    default:
        break;
    }

    switch (couple_ui_view()) {
    case COUPLE_VIEW_HOME:
    case COUPLE_VIEW_MISS_SENT:
    case COUPLE_VIEW_MISS_RECV:
    case COUPLE_VIEW_HUG_RECV:
        send_miss();
        break;
    default:
        clear_transient();
        return_home();
        break;
    }
}

/* OK 键的武装计时到点：按 PRESS 分支定的时间（普通页 APP_HUG_ARM_MS、
   解绑页 APP_UNPAIR_HOLD_MS）还没松手 → 进抱抱（或退出流程页/确认解绑）。 */
static void handle_ok_arm(void)
{
    if (s_ok_press_ms == 0u) {
        return; /* 已经松手了：这次按压由 RELEASE 收尾 */
    }
    const uint32_t held = now_ms() - s_ok_press_ms;

    if (s_flow == APP_FLOW_UNPAIR) {
        /* 唯一的例外：解绑是破坏性操作，屏幕写的是「按住 OK 确认解除」，
           所以这里长按才是确认。PRESS 分支已把武装时间放长到
           APP_UNPAIR_HOLD_MS，能在 1.5 秒前松手的是反悔，走 RELEASE。
           消费掉按压，松手不再触发别的。 */
        s_ok_press_ms = 0u;
        ESP_LOGI(TAG, "按键: OK 按住 %ums → 确认解除配对", (unsigned)held);
        do_unpair();
        return;
    }

    if (s_flow != APP_FLOW_NONE) {
        /* 流程页里长按 = 退出；消费掉这次按压，松手不再算「确认」。 */
        s_ok_press_ms = 0u;
        ESP_LOGI(TAG, "按键: OK 按住 %ums → 退出流程", (unsigned)held);
        escape_to_home();
        return;
    }

    ESP_LOGI(TAG, "按键: OK 按住 %ums → 抱抱", (unsigned)held);
    if (!start_hug(s_ok_press_ms)) {
        s_ok_press_ms = 0u; /* 没配对上：消费掉，别在松手时再发一次想你 */
    }
}

static void handle_key(uint8_t btn, uint8_t ev)
{
    const uint32_t now = now_ms();

    /* 任何按键都算「有人在」：即使下面被吞掉，也不该马上又睡回去。 */
    s_last_input_ms = now;

    if (couple_ui_sleeping()) {
        /* 第一下只负责亮屏：不前进页面、不触发想你/抱抱。 */
        couple_ui_wake();
        s_wake_guard_until_ms = now + APP_WAKE_GUARD_MS;
        return;
    }

    /* 唤醒时那一次按压松手 185ms 后补发的 CLICK 不能当用户操作；PRESS/RELEASE 必须放行。 */
    if ((bsp_btn_ev_t)ev == BSP_BTN_CLICK && ms_pending(now, s_wake_guard_until_ms)) {
        return;
    }

    if (btn == (uint8_t)BSP_BTN_OK) {
        switch ((bsp_btn_ev_t)ev) {
        case BSP_BTN_PRESS: {
            /* 记下按下时刻并武装计时；满 APP_HUG_ARM_MS 仍按着才算抱抱。
               解绑页是唯一的例外：那是不可逆操作，武装时间单独放长到
               APP_UNPAIR_HOLD_MS，屏幕上的「按住 OK 确认解除」才有分量。 */
            const uint32_t arm_ms =
                (s_flow == APP_FLOW_UNPAIR) ? APP_UNPAIR_HOLD_MS : APP_HUG_ARM_MS;
            s_ok_press_ms = (now != 0u) ? now : 1u;
            if (s_press_timer != NULL) {
                (void)esp_timer_start_once(s_press_timer, (uint64_t)arm_ms * 1000u);
            }
            return;
        }

        case BSP_BTN_RELEASE: {
            const bool armed = (s_ok_press_ms != 0u);
            const uint32_t held = armed ? (now - s_ok_press_ms) : 0u;
            const bool hugging = s_hug_active;
            s_ok_press_ms = 0u;
            if (s_press_timer != NULL) {
                (void)esp_timer_stop(s_press_timer);
            }
            if (hugging) {
                end_hug();
                return;
            }
            if (armed) {
                ESP_LOGI(TAG, "按键: OK 短按 %ums → 想你/确认", (unsigned)held);
                ok_click();
            }
            return;
        }

        default:
            /* CLICK / LONG / DOUBLE 都不参与 OK 判定：唯一真源是 PRESS/RELEASE。 */
            return;
        }
    }

    /* 只剩下 UP / DOWN。 */
    if ((bsp_btn_ev_t)ev != BSP_BTN_CLICK) {
        return;
    }
    if (s_hug_active) {
        return;
    }

    const bool up = (btn == (uint8_t)BSP_BTN_UP);
    const int step = up ? -1 : 1;

    if (s_flow != APP_FLOW_NONE) {
        switch (s_flow) {
        case APP_FLOW_MENU: {
            const uint8_t total = couple_ui_menu_count();
            if (total > 0u) {
                int next = (int)s_menu_sel + step;
                while (next < 0) {
                    next += total;
                }
                s_menu_sel = (uint8_t)(next % total);
                couple_ui_show_menu(s_menu_sel);
            }
            break;
        }
        case APP_FLOW_PAIR_CHOICE:
            enter_pair_join();
            break;
        case APP_FLOW_PAIR_JOIN:
            pair_entry_adjust(step);
            break;
        default:
            break;
        }
        return;
    }

    switch (couple_ui_view()) {
    case COUPLE_VIEW_HOME:
        if (up) {
            enter_daily();
        } else {
            enter_menu();
        }
        break;
    case COUPLE_VIEW_DAILY:
        clear_transient();
        return_home();
        break;
    default:
        break;
    }
}

/* ---------- 1 Hz ---------- */

static void on_tick(void)
{
    const uint32_t now = now_ms();
    const uint32_t utc = couple_net_clock_now_utc();

    couple_store_tick(now);

    if (utc != 0u && couple_store_roll_day(utc)) {
        /* 新的一天：让「今天第 N 次想你」有机会再响一次。 */
        s_burst_notice_shown = false;
    }

    /* 电量计读一次要几十毫秒，别跟 1 Hz 绑在一起。 */
    if ((uint32_t)(now - s_battery_ms) >= APP_BATTERY_INTERVAL_MS) {
        s_battery_ms = now;
        const int soc = bsp_battery_soc();
        if (soc < 0) {
            s_battery_percent = -1;
        } else {
            s_battery_percent = (soc > 100) ? 100 : soc;
        }
    }

    /*
     * 内存长线打点。系统堆和 LVGL 池是两块独立内存：调大 CONFIG_LV_MEM_SIZE_KILOBYTES
     * 就是直接从系统堆里挖走，所以两边必须同时盯。min 是从开机到现在的历史最低值，
     * 比瞬时值更能说明「是不是已经贴着地板在飞」。LVGL 池那一半在 couple_ui 的每秒
     * tick 里打印（那边本来就持着 LVGL 锁）。
     */
    if ((uint32_t)(now - s_mem_ms) >= APP_MEM_LOG_INTERVAL_MS) {
        s_mem_ms = now;
        ESP_LOGI(TAG, "[mem] sys free=%u largest=%u min=%u", (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
                 (unsigned)esp_get_minimum_free_heap_size());
    }

    /* 配对码有效期倒计时：每秒重刷一次，屏幕上的秒数才不会跳。 */
    if (s_flow == APP_FLOW_PAIR_HOST && s_pair_code[0] != '\0' && s_pair_deadline_ms != 0u) {
        if (ms_elapsed(now, s_pair_deadline_ms)) {
            s_pair_deadline_ms = 0u;
        } else {
            const uint32_t remain_ms = s_pair_deadline_ms - now;
            couple_ui_show_pair_offer(s_pair_code, (remain_ms + 999u) / 1000u);
        }
    }

    if (s_flow == APP_FLOW_PAIR_DONE && ms_elapsed(now, s_pair_done_ms)) {
        s_pair_done_ms = 0u;
        escape_to_home();
    }

    if (s_flow == APP_FLOW_PROV) {
        handle_prov_tick(now);
    }

    /* 临时页到点自己回首页。sticky（until == 0）永远不自己走。 */
    if (s_transient_active && !s_hug_synced && s_transient_until_ms != 0u &&
        ms_elapsed(now, s_transient_until_ms)) {
        clear_transient();
        if (s_flow == APP_FLOW_PROV) {
            s_prov_state = APP_PROV_IDLE;
            s_flow = APP_FLOW_NONE;
        }
        return_home();
    }

    /*
     * 闲置关背光。不看当前在哪一页：只要没有进行中的流程、没有在抱抱，
     * 而且最近 30 秒既没有按键也没有对方发来的互动，就熄屏。
     * 已经睡了就不再重复调用（couple_ui_sleep() 自己也是幂等的）。
     */
    if (!couple_ui_sleeping() && s_flow == APP_FLOW_NONE && !s_hug_active &&
        (uint32_t)(now - s_last_input_ms) >= APP_SLEEP_IDLE_MS) {
        couple_ui_sleep();
    }

    refresh_status();
}

/* ---------- 任务与分派 ---------- */

static void handle_net_state(void)
{
    refresh_status();
}

static void dispatch(app_msg_t *msg)
{
    switch ((app_msg_kind_t)msg->kind) {
    case APP_MSG_KEY:
        handle_key(msg->u.key.btn, msg->u.key.ev);
        break;

    case APP_MSG_TICK:
        on_tick();
        break;

    case APP_MSG_HOLD_TICK:
        if (s_hug_active) {
            on_hold_tick();
        }
        break;

    case APP_MSG_OK_ARM:
        handle_ok_arm();
        break;

    case APP_MSG_NET_STATE:
        handle_net_state();
        break;

    case APP_MSG_NET_SYNC:
        handle_sync(msg);
        break;

    case APP_MSG_NET_PAIR:
        handle_pair(&msg->u.pair);
        break;

    case APP_MSG_PROV_SAVED:
        handle_prov_saved(msg);
        break;

    case APP_MSG_PROV_STATUS:
        refresh_prov_ap();
        break;

    case APP_MSG_QUIT:
    default:
        break;
    }
}

static void app_task(void *arg)
{
    (void)arg;

    app_msg_t msg;
    for (;;) {
        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if ((app_msg_kind_t)msg.kind == APP_MSG_QUIT) {
            break;
        }
        dispatch(&msg);
    }

    s_task_exited = true;
    vTaskDelete(NULL);
}

/* ---------- couple_net 回调（跑在 net worker，只许拷贝 + 入队） ---------- */

static void on_state(couple_net_state_t state, void *user)
{
    (void)user;
    ESP_LOGD(TAG, "net 状态 -> %d", (int)state);

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_NET_STATE;
    msg.u.net_state = (uint8_t)state;
    (void)post(&msg);
}

static void on_sync(const couple_net_sync_t *sync, void *user)
{
    (void)user;
    if (sync == NULL) {
        return;
    }

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_NET_SYNC;
    msg.u.sync.partner_online = sync->partner_online;
    msg.u.sync.partner_holding = sync->partner_holding;
    msg.u.sync.pair_state = sync->pair_state;
    if (sync->events != NULL) {
        msg.u.sync.events = *sync->events; /* 值拷贝；事件列表在回调返回后失效 */
    }
    (void)post(&msg);
}

static void on_pair(const couple_net_pair_event_t *ev, void *user)
{
    (void)user;
    if (ev == NULL) {
        return;
    }

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_NET_PAIR;
    msg.u.pair = *ev;
    (void)post(&msg);
}

/* ---------- esp_timer 回调（只投递，不画界面） ---------- */

static void tick_timer_cb(void *arg)
{
    (void)arg;
    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_TICK;
    (void)post(&msg);
}

static void hold_timer_cb(void *arg)
{
    (void)arg;
    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_HOLD_TICK;
    (void)post(&msg);
}

/* OK 按住的武装计时到点（时长由 PRESS 分支定）。跑在 ESP_TIMER_TASK 上，
   只投消息，不碰任何 app 状态。 */
static void press_timer_cb(void *arg)
{
    (void)arg;
    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_OK_ARM;
    (void)post(&msg);
}

/* ---------- 生命周期 ---------- */

esp_err_t couple_app_init(void)
{
    if (s_started) {
        return ESP_OK;
    }

    s_shutting_down = false;
    s_task_exited = false;

    /* 从「刚刚有人」开始闲置计时。必须放在任务与 tick 定时器创建之前：否则开机
     * 那一拍会拿静态初值 0 去算闲置（uptime 早已超过 30s 时会当场熄屏）。
     * 这里不需要再调 couple_ui_wake()：背光已由 main.c 设成 100，而稍后的
     * couple_ui_show(HOME) → show_view() 会顺带把 couple_ui 的 s_sleeping 拉回 false。 */
    s_last_input_ms = now_ms();

    esp_err_t err = couple_store_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "存储初始化失败: %s", esp_err_to_name(err));
    }
    /* 必须在 store 初始化之后问：擦除重建是它做的。 */
    const bool nvs_wiped = couple_nvs_was_wiped();

    s_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_msg_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "队列创建失败");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(app_task, APP_TASK_NAME, APP_TASK_STACK, NULL, APP_TASK_PRIO, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "任务创建失败");
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    if (couple_wifi_begin() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败，先按离线运行");
    }

    memset(&s_net_hooks, 0, sizeof(s_net_hooks));
    s_net_hooks.on_state = on_state;
    s_net_hooks.on_sync = on_sync;
    s_net_hooks.on_pair = on_pair;
    s_net_hooks.user = NULL;
    if (couple_net_start(&s_net_hooks) != ESP_OK) {
        ESP_LOGW(TAG, "网络层启动失败，界面会停在离线");
    }

    /* esp_timer_create_args_t 逐字段赋值，避免 -Wmissing-field-initializers。 */
    esp_timer_create_args_t tick_args;
    memset(&tick_args, 0, sizeof(tick_args));
    tick_args.callback = tick_timer_cb;
    tick_args.dispatch_method = ESP_TIMER_TASK;
    tick_args.name = "couple_tick";
    if (esp_timer_create(&tick_args, &s_tick_timer) != ESP_OK) {
        ESP_LOGE(TAG, "tick 定时器创建失败");
        s_tick_timer = NULL;
    }

    esp_timer_create_args_t hold_args;
    memset(&hold_args, 0, sizeof(hold_args));
    hold_args.callback = hold_timer_cb;
    hold_args.dispatch_method = ESP_TIMER_TASK;
    hold_args.name = "couple_hold";
    if (esp_timer_create(&hold_args, &s_hold_timer) != ESP_OK) {
        ESP_LOGE(TAG, "抱抱定时器创建失败");
        s_hold_timer = NULL;
    }

    esp_timer_create_args_t press_args;
    memset(&press_args, 0, sizeof(press_args));
    press_args.callback = press_timer_cb;
    press_args.dispatch_method = ESP_TIMER_TASK;
    press_args.name = "couple_press";
    if (esp_timer_create(&press_args, &s_press_timer) != ESP_OK) {
        ESP_LOGE(TAG, "抱抱武装定时器创建失败");
        s_press_timer = NULL;
    }

    if (s_tick_timer != NULL) {
        if (esp_timer_start_periodic(s_tick_timer, (uint64_t)COUPLE_HOME_TICK_MS * 1000u) != ESP_OK) {
            ESP_LOGW(TAG, "tick 定时器启动失败");
        }
    }

    /* 复位流程状态：无论此前发生过什么，都从首页开始。 */
    s_flow = APP_FLOW_NONE;
    s_menu_sel = 0u;
    s_pair_filled = 0u;
    s_pair_code[0] = '\0';
    s_pair_deadline_ms = 0u;
    s_pair_done_ms = 0u;
    s_partner_name[0] = '\0';
    s_hug_active = false;
    s_hug_synced = false;
    s_loop_owned = false;
    s_hug_tier = COUPLE_HUG_TIER_NONE;
    s_wake_guard_until_ms = 0u;
    s_ok_press_ms = 0u;
    s_prov_state = APP_PROV_IDLE;
    s_prov_clients = 0u;
    s_prov_ap_drawn = false;
    s_prov_deadline_ms = 0u;
    s_partner_online = false;
    s_peer_holding = false;
    s_burst_notice_shown = false;
    clear_transient();

    s_battery_ms = 0u;
    const int soc = bsp_battery_soc();
    s_battery_percent = (soc < 0) ? -1 : ((soc > 100) ? 100 : soc);

    couple_ui_show(COUPLE_VIEW_HOME);
    refresh_status();

    if (nvs_wiped) {
        couple_ui_toast(COUPLE_STR_NVS_RESET_TOAST);
    }

    ESP_LOGI(TAG, "已启动: paired=%d rel=%s", (int)couple_store_is_paired(),
             (couple_store_is_paired() ? "yes" : "no"));

    s_started = true;
    return ESP_OK;
}

bool couple_app_started(void)
{
    return s_started;
}

void couple_app_handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (s_shutting_down) {
        return;
    }

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_KEY;
    msg.u.key.btn = (uint8_t)btn;
    msg.u.key.ev = (uint8_t)ev;

    if (!post(&msg)) {
        ESP_LOGW(TAG, "输入队列满，丢弃按键 (btn=%d ev=%d) 累计 %u", (int)btn, (int)ev,
                 (unsigned)s_dropped_keys);
    }
}

esp_err_t couple_app_shutdown(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    s_shutting_down = true;

    if (s_tick_timer != NULL) {
        (void)esp_timer_stop(s_tick_timer);
    }
    if (s_hold_timer != NULL) {
        (void)esp_timer_stop(s_hold_timer);
    }
    if (s_press_timer != NULL) {
        (void)esp_timer_stop(s_press_timer);
    }

    /*
     * 还在抱抱里就被要求关机：把这次抱抱如实写进队列。
     *
     * 说明：紧随其后的 couple_net_stop() 会清空 outbox，所以这一条基本发不出去，
     * couple_net.c 会为丢弃打一条 WARN。要让对面一定收到，就得在停网前留出同步
     * 时间，代价是每次关机多等几秒 —— 不划算：relay 上本机的 holding 标志会自行
     * 超时，对面不会一直挂着。这里保留入队是为了走正常的计分与状态收尾路径。
     */
    if (s_hug_active) {
        uint32_t held = now_ms() - s_hug_start_ms;
        if (held > COUPLE_HUG_MAX_MS) {
            held = COUPLE_HUG_MAX_MS;
        }
        couple_event_t ev;
        memset(&ev, 0, sizeof(ev));
        couple_event_set_u16(&ev, (uint8_t)COUPLE_EVENT_HUG_END, (uint16_t)held);
        (void)couple_net_enqueue_event(&ev);
        couple_net_set_holding(false);
        s_hug_active = false;
        s_hug_tier = COUPLE_HUG_TIER_NONE;
        s_hug_synced = false;
    }
    if (s_loop_owned) {
        couple_audio_stop_loop();
        s_loop_owned = false;
    }

    if (couple_prov_active()) {
        couple_prov_stop();
    }

    if (couple_net_stop() != ESP_OK) {
        ESP_LOGW(TAG, "网络层没能在超时内停下");
        s_shutting_down = false;
        return ESP_ERR_TIMEOUT;
    }

    if (!couple_audio_quiesce(APP_QUIESCE_MS)) {
        ESP_LOGW(TAG, "音频还在响，不保证能进深睡");
        s_shutting_down = false;
        return ESP_ERR_TIMEOUT;
    }

    if (couple_store_flush() != ESP_OK) {
        ESP_LOGW(TAG, "最后一次落盘失败");
    }

    app_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = APP_MSG_QUIT;
    (void)xQueueSend(s_queue, &msg, 0);

    for (uint32_t waited = 0u; waited < APP_QUIT_WAIT_MS; waited += 10u) {
        if (s_task_exited) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_task_exited) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        s_task = NULL;
    } else {
        ESP_LOGW(TAG, "app 任务没能及时退出，保留队列以免野指针");
    }

    s_started = false;
    s_shutting_down = false;
    return ESP_OK;
}