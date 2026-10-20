#pragma once

/*
 * Heartlink 情侣心动终端 —— 编译期可调参数。
 *
 * 需要改默认 relay 地址、时区或各类时间窗口时改这里。
 * 运行期可被 NVS 中的用户设置覆盖的项，在注释中单独标注。
 */

#define COUPLE_FW_VERSION "1.0.0"

/* relay 基地址占位符：请改成你自己的 relay 主机，例如 "http://relay.example.com"。
 * 运行期可在配网页覆盖（存 NVS）；只在编译期换过地址时才需要重新烧录。
 * 必须写 http:// —— 固件里没有 TLS 栈，写 https:// 会一直按网络故障重试。 */
#define COUPLE_DEFAULT_RELAY_URL "http://relay.example.com"

/* 本机自报名字的默认值：配对时发给 relay，对端配对成功后显示的就是它。
 * 可以在配网页覆盖，改完存 NVS。
 * 注意：这个值只对「NVS 里还没存过名字」的机器生效；已经配过网的机器改名
 * 走 couple_store.c 里的旧值迁移分支，不是改这里。 */
#define COUPLE_DEFAULT_SELF_NAME "lover"

/* 时区偏移（分钟）。东八区 = 480。用于「今天」的日界线。 */
#define COUPLE_UTC_OFFSET_MINUTES 480

/* ---------- 时间 ---------- */

#define COUPLE_DAY_SECONDS 86400
#define COUPLE_RELATIVE_JUST_NOW_SEC 60

/* ---------- 网络 ---------- */

#define COUPLE_SYNC_LONGPOLL_MS 25000 /* 与服务端约定的长轮询上限 */
/*
 * HTTP 整体超时。
 * relay 的 /v1/sync 长轮询是 8 秒，它经 Cloudflare 隧道写出应答大约再晚 1.9 秒，
 * 一轮往返约 10 秒；20 秒因此留出约 10 秒余量，正常往返不会被本机掐断，同时给重试
 * 留出空间。
 * 注：COUPLE_SYNC_LONGPOLL_MS(25s) 是「与服务端约定的上限」，不是 relay 实际等待时长。
 */
#define COUPLE_HTTP_TIMEOUT_MS 20000
#define COUPLE_RETRY_BASE_MS 2000
#define COUPLE_RETRY_MAX_MS 60000

/* Wi-Fi 关联重连的退避（驱动不会自动重连，由 couple_wifi 自己实现）。 */
#define COUPLE_WIFI_RETRY_BASE_MS 2000
#define COUPLE_WIFI_RETRY_MAX_MS 60000

#define COUPLE_PRESENCE_STALE_SEC 45
#define COUPLE_TIME_SYNC_INTERVAL_SEC 300 /* 与服务端对时的间隔 */

/*
 * ---------- 同步计数器（防重放）的持久化 ----------
 *
 * seq 在运行期只改内存、不落盘（25 秒一次长轮询，若每次都写，一天就是几千次擦写）。
 * 为了重启后仍然单调，加载时把 seq 抬到「已落盘值 + BOOT_MARGIN」，并立刻落盘一次；
 * 运行期每前进 PERSIST_STEP 才标一次脏（由 couple_store_tick 顺带写入）。
 *
 * 不变的约束：PERSIST_STEP 必须明显小于 BOOT_MARGIN。
 * 否则异常掉电后重启的起点可能低于本次会话已经用过的 seq，对端会按重放丢弃。
 */
#define COUPLE_SEQ_BOOT_MARGIN 1024
#define COUPLE_SEQ_PERSIST_STEP 128

/* ---------- 互动参数 ---------- */

#define COUPLE_MISS_WINDOW_MS 10000  /* 心有灵犀判定窗口 */
#define COUPLE_HUG_TIER_SMALL_MS 700 /* 小抱抱下限 */
#define COUPLE_HUG_TIER_BIG_MS 3000  /* 大抱抱下限 */
#define COUPLE_HUG_TIER_CLINGY_MS 8000 /* 不撒手抱抱下限 */
#define COUPLE_HUG_MAX_MS 60000        /* 上报时长上限 */

#define COUPLE_OUTBOX_CAPACITY 32      /* 离线待发队列容量 */

/*
 * 「今天第 N 次想你」这条彩蛋文案的触发阈值。
 * 判定口径：当天双向 day_misses 恰好等于该值时必定触发一次（只触发一次，
 * 因为一天之内 day_misses 只会单调经过这个数）。文案里的数字跟着这个常量走，
 * 调这个值等于同时改文案，不需要动 couple_app.c 里的字符串。
 */
#define COUPLE_MISS_BURST_NOTICE 7

/* 抱抱按住期间的界面刷新节拍（20 Hz）。 */
#define COUPLE_HUG_HOLD_TICK_MS 50

/* ---------- Love Energy ---------- */

#define COUPLE_ENERGY_MISS_SENT 1
#define COUPLE_ENERGY_MISS_RECEIVED 1
#define COUPLE_ENERGY_HEART_SYNC 5
#define COUPLE_ENERGY_HUG_SYNC 5
#define COUPLE_ENERGY_DAILY_STREAK 3 /* 每日连续互动奖励 */

/* ---------- 持久化 ---------- */

#define COUPLE_NVS_FLUSH_INTERVAL_MS 20000 /* 批量落盘间隔 */
#define COUPLE_NVS_FLUSH_DIRTY_LIMIT 8     /* 脏标记到该数量提前落盘 */

/* ---------- UI ---------- */

#define COUPLE_TOAST_MS 2600        /* 顶部提示停留时间 */
#define COUPLE_HOME_TICK_MS 1000    /* 首页刷新间隔 */

/* ---------- 音效 ---------- */

/*
 * 音量（0..100）。bsp_audio 的初始音量是 0，couple_audio_init() 会显式设一次，
 * 所以这个值决定开机后第一声有没有声音。
 */
#define COUPLE_AUDIO_VOLUME 80

/*
 * 每块渲染的采样数（16000 Hz 下 1024 采样 = 64 ms）。
 * 它同时决定两件事：worker 的静态 PCM 缓冲大小（2 x CHUNK x 2 字节），
 * 以及「停止/取消」的最坏生效延迟。调大省任务开销但响应变钝。
 */
#define COUPLE_AUDIO_CHUNK_SAMPLES 1024

/*
 * 双向抱抱循环心跳两拍之间插入的静音时长。
 * 不插静音的话 I2S 会重复最后一块 DMA 数据，心跳听起来像持续嗡嗡声。
 */
#define COUPLE_AUDIO_LOOP_GAP_MS 120

/* 音频 worker 的消息队列深度。音效只入队不阻塞，满了就丢弃并返回 false。 */
#define COUPLE_AUDIO_QUEUE_DEPTH 8