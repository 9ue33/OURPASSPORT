#pragma once

/*
 * Heartlink 情侣心动终端 —— 界面层。
 *
 * 职责边界（很重要）：
 *   本模块是**纯被动的渲染器**。它不读按键、不碰网络、不碰 NVS、不放音效。
 *   状态机与输入解码全在 couple_app；couple_app 通过下面这组 push/show 函数
 *   把「现在该长什么样」告诉 UI，UI 只负责把它画得好看。
 *
 *   这样做的直接好处：UI 里不存在任何阻塞或异步等待，LVGL 回调里永远不会
 *   碰到网络/Flash/音频；同时 couple_app 可以完全脱离 LVGL 做宿主测试。
 *
 * 线程模型：
 *   下面每个公开函数内部都会自己拿 bsp_lvgl_lock()，因此可以从任意任务调用，
 *   调用方不需要（也不应该）提前持锁。
 *   esp_lvgl_port 的锁是可重入的，所以提前持锁也不会死锁，但没必要。
 *   UI 内部用 lv_timer 驱动动画与每秒文案刷新，那些回调跑在 LVGL 任务里，
 *   天然安全，不需要再加锁。
 *
 * 本模块不使用任何位图资源：心形、光标、进度条全部由 LVGL 的矩形对象拼成
 * （像素掌机风），因此不占 Flash，也不需要额外的字体字形。
 */

#include "couple_types.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

/* ---------- 页面 ---------- */

typedef enum {
    COUPLE_VIEW_HOME = 0,   /* 首页：OUR LINK */
    COUPLE_VIEW_MISS_SENT,  /* 已发出想你（轻确认） */
    COUPLE_VIEW_MISS_RECV,  /* 收到想你：全屏心形由小到大 */
    COUPLE_VIEW_HUG_HOLD,   /* 本机正在按住 OK */
    COUPLE_VIEW_HUG_RECV,   /* 收到对方的抱抱 */
    COUPLE_VIEW_HUG_SYNC,   /* HUG CONNECTED：双向同步心跳 */
    COUPLE_VIEW_HEART_SYNC, /* HEART SYNC：心有灵犀全屏 */
    COUPLE_VIEW_MILESTONE,  /* 里程碑彩蛋 */
    COUPLE_VIEW_DAILY,      /* 每日统计 */
    COUPLE_VIEW_MENU,       /* 三项菜单：配对 / 配网 / 解绑 */
    COUPLE_VIEW_PAIR,       /* 配对 / 加入 / 解绑 */
    COUPLE_VIEW_PROV,       /* Wi-Fi 配网 */
    COUPLE_VIEW_COUNT
} couple_view_t;

/* ---------- 常驻状态 ---------- */

/*
 * 首页、每日页、抱抱页都要用的字段。couple_app 在状态变化时推一次即可，
 * 页面内部的时间文案（"3 分钟前"）由 UI 自己的 lv_timer 每秒重算。
 */
typedef struct {
    bool paired;      /* 已绑定 */
    bool partner_known; /* partner_name 有效 */
    bool partner_online;
    bool wifi_connected;

    int8_t battery_percent; /* 0..100；-1 = 电量计读不到 */
    int8_t wifi_rssi;       /* dBm；0 = 未知 */

    uint32_t now_utc; /* 0 = 尚未对时，此时不显示绝对时间 */

    uint32_t energy;
    uint32_t total_interactions;

    uint16_t day_misses; /* 今日想你（双向） */
    uint16_t day_hugs;   /* 今日抱抱（双向） */
    uint16_t streak_days;
    uint32_t day_first_interaction_utc; /* 0 = 今天还没互动 */

    uint32_t last_miss_received_utc; /* 0 = 还没收到过 */

    char partner_name[COUPLE_NAME_MAX + 1u];
} couple_ui_status_t;

/* ---------- 生命周期 ---------- */

/*
 * 建好首页与顶部横幅并把首页推上屏幕；其余页面首次进入时按需构建，离开时释放。
 * 必须在 bsp_lvgl_init() 成功之后调用。幂等：重复调用直接返回 ESP_OK。
 */
esp_err_t couple_ui_init(void);

/* ---------- 背光休眠 ---------- */

/*
 * 休眠：关背光（0%）。页面对象、动画与 LVGL 定时器保持不动，下一次
 * show_view() 或 couple_ui_wake() 会原样恢复显示。
 * 不碰任何 LVGL 对象，因此不取 bsp_lvgl_lock()，可以从任意任务调用。
 */
void couple_ui_sleep(void);

/* 唤醒：把背光恢复到 100%。未睡时无副作用。 */
void couple_ui_wake(void);

/* 当前是否处于休眠（背光熄灭）状态。不加锁，任何任务都能读。 */
bool couple_ui_sleeping(void);

/* 推一次常驻状态（首页/每日页/抱抱页立即重绘）。 */
void couple_ui_push_status(const couple_ui_status_t *status);

/* 当前页面。 */
couple_view_t couple_ui_view(void);

/* 切到某个基础页面（其余带参数的页面请用各自的 show_* 函数）。 */
void couple_ui_show(couple_view_t view);

/* ---------- 各页面的进入接口 ---------- */

/*
 * 收到想你。flavor 是彩蛋文案（可为 NULL，UI 会退化成默认文案）。
 * 进入时会播放「心形由小到大」的放大动画。
 */
void couple_ui_show_miss_received(const char *flavor);

/* 本机刚发出想你。 */
void couple_ui_show_miss_sent(void);

/*
 * 按住 OK 时的实时更新，由 couple_app 的输入任务按帧调用（建议 20 Hz 左右）。
 * tier 变化时 UI 会做一次跳变强调；held_ms 用于进度环与文案。
 */
void couple_ui_show_hug_hold(uint32_t held_ms, couple_hug_tier_t tier);

/* 对方也在抱 → HUG CONNECTED（同步心跳）。 */
void couple_ui_show_hug_sync(void);

/* 对方的抱抱到了（单方面）。held_ms 来自 HUG_END 的 value。 */
void couple_ui_show_hug_received(uint16_t held_ms);

/* 心有灵犀全屏。 */
void couple_ui_show_heart_sync(void);

/* 里程碑彩蛋。index 越界时退化为一次 toast。 */
void couple_ui_show_milestone(int index);

/*
 * 「你不在的时候，TA想了你3次。」count == 0 时不切页。
 */
void couple_ui_show_offline_summary(uint16_t miss_count);

/* 顶部横幅文案（随机彩蛋）。text 为 NULL 或空串时清空横幅。 */
void couple_ui_toast(const char *text);

/* 更新后的首页是否已经被推过一次（couple_app 用它决定要不要先回首页）。 */
bool couple_ui_home_ready(void);

/* ---------- 菜单 ---------- */

/* 菜单项数量（配对 / 配网 / 解绑）。 */
uint8_t couple_ui_menu_count(void);

/* 显示菜单并高亮第 selected 项（越界会被夹紧）。 */
void couple_ui_show_menu(uint8_t selected);

/* ---------- 配对页 ---------- */

/*
 * 本机发起配对：显示 6 位分享码与剩余有效期。ttl_secs 为 0 时只显示码。
 */
void couple_ui_show_pair_offer(const char *code, uint32_t ttl_secs);

/*
 * 本机加入配对：逐位输入对方屏幕上的 6 位码。
 * digits 是被编辑的 6 个数字，filled 是已确认的位数，cursor 是当前编辑位。
 */
void couple_ui_show_pair_entry(const uint8_t *digits, uint8_t filled, uint8_t cursor);

/*
 * 配对过程中的文字状态。
 *   busy = true  转圈（等待对方 / 提交中）
 *   busy = false 定格，ok 决定配色（成功 / 失败）
 */
void couple_ui_show_pair_status(const char *headline, const char *detail, bool busy, bool ok);

/* 配对成功：显示对方名字。 */
void couple_ui_show_pair_confirmed(const char *partner_name);

/* 解绑确认页。 */
void couple_ui_show_unpair_prompt(const char *partner_name);

/* ---------- 配网页 ---------- */

/*
 * 显示 SoftAP 配网信息。ssid/pass 为 AP 凭据，ip 为配置页地址（如 "192.168.4.1"），
 * clients 为当前连上来的设备数（0 时显示等待）。
 */
void couple_ui_show_prov_ap(const char *ssid, const char *pass, const char *ip, uint8_t clients);

/* 配网/联网的文字状态（保存中、连接中、成功、失败）。 */
void couple_ui_show_prov_status(const char *headline, const char *detail, bool busy, bool ok);