#pragma once

/*
 * Heartlink 情侣心动终端 —— 与 relay 通信的唯一出口。
 *
 * 职责：
 *   - 持有 STA 日常模式下的同步循环：POST <relay>/v1/sync，长轮询收发事件
 *   - 持有配对流程：begin / await / claim / unpair
 *   - 维护「现在是几点」（用服务端 server_time 对时，见 couple_net_clock_now_utc）
 *   - 把待发事件排进一个 32 条的离线队列（outbox），联网后按序补发
 *
 * 不负责：Wi-Fi 关联与重连（那是 couple_wifi 的事）、NVS 读写（couple_store）、
 * 界面与状态机（couple_state / UI）。本模块零 LVGL 依赖。
 *
 * ============================ 任务模型 ============================
 *
 * start 之后只有一个 worker 任务（栈 6144、优先级 5）在跑：
 *
 *   任意任务                    worker 任务
 *   --------                    -----------
 *   enqueue_event() -> outbox   取事件 + 组帧 + HTTP + 解析 + 回调
 *   set_holding() (标志)        pairing 状态机
 *   pair_*() 与 unpair() -> 命令队列  Wi-Fi 事件驱动的重新求值
 *   clock_now_utc() (临界区读)
 *
 * 入队事件、改抱抱状态还会顺手「掐断」worker 正在跑的那一轮 /v1/sync 长轮询
 * （shutdown 掉它的 socket）并投一条唤醒命令，所以新事件是毫秒级离开设备的，
 * 不必等这一轮长轮询走完。只有 /v1/sync 会被掐断，配对/解绑的在途请求不受影响。
 * 细节与耐久性缺口见 couple_net.c 顶部的「已知取舍」。
 *
 * 所有回调（on_state / on_sync / on_pair）都在 worker 任务上下文里同步调用：
 *   - 回调里禁止阻塞：不得调用 LVGL、不得写 Flash、不得做网络/音频、不得打长日志。
 *     需要做这些事的话，回调里只置个标志或投个队列，交给别的任务。
 *   - 回调里不得再调用 couple_net_*（除 clock_now_utc / state 这类纯读），
 *     尤其不得调用 couple_net_stop()（会等自己退出而永久卡住）。
 *   - 回调参数里的指针（事件列表、配对结构）只在本次回调期间有效，不要存起来。
 *
 * stop 会等在途的那一次 HTTP 走完（长轮询最长约 COUPLE_SYNC_LONGPOLL_MS，
 * 所以最坏要等几十秒），不要在持锁或中断上下文中调用。
 *
 * ============================ 语义上的取舍 ============================
 *
 * 1) 一次同步要花多久
 *    relay 用长轮询挂住每个 /v1/sync（最长约 8 秒）来省电、少发包。本机侧
 *    「刚入队的事件」不按这个节奏等：入队/改抱抱状态会掐断在飞的那一轮并立刻
 *    重发，事件在毫秒级就出去了；反之，没有新事件时 worker 就安静地挂在长轮询上。
 *    界面上的即时反馈仍然应该走本地状态，不要等网络确认。
 *
 * 2) outbox 满了怎么办
 *    outbox 固定 32 条、入队绝不阻塞。满了丢最旧的一条并记一条警告日志
 *    （丢最近的事件会让用户觉得「按了没反应」，丢最旧的则只是补发不完整）。
 *
 * 3) 对时
 *    couple_net_clock_now_utc() = 最近一次 server_time + 之后单调流逝的时间。
 *    从未对过时返回 0，调用方必须处理这个情况（别把它当 1970 年用）。
 *
 * 4) 传输
 *    只支持 http://（relay 部署在局域网/内网机器上）。relay 地址写成 https://
 *    时本模块不做 TLS，会连接失败并按网络故障退避重试。
 *
 * ============================ 使用前提 ============================
 *
 *   - couple_store_init() 之后才能 start
 *   - 要先 couple_wifi_begin()（本模块只负责切 STA 与下发凭据）
 *   - 凭据/relay 地址在运行期被改过（配网完成、设置页保存）后，调
 *     couple_net_settings_changed() 让它重新求值
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "couple_types.h"

/* 共享密钥长度跟着协议走，这里只是不再把 couple_proto.h 拉进公共头。 */
#define COUPLE_NET_PAIR_KEY_LEN COUPLE_PAIR_TOKEN_LEN

/*
 * 本模块自己定义的两个状态码，用于区分「没网/对端没答」和「本地条件不满足」。
 * 它们不占 couple_status_t（0..7）的值域，可以安全地和协议状态码共用一个 uint8_t。
 */
#define COUPLE_NET_STATUS_NETWORK 0x80u /* 连不上、HTTP 出错、帧校验失败 */
#define COUPLE_NET_STATUS_LOCAL 0x81u   /* 没启动 / 没 IP / 没配对 */

/* ---------- 连接状态 ---------- */

typedef enum {
    COUPLE_NET_STATE_IDLE = 0,     /* 没存 Wi-Fi 凭据，还没走过配网 */
    COUPLE_NET_STATE_WIFI_WAIT,    /* 有凭据，但还没拿到 IP */
    COUPLE_NET_STATE_ONLINE,       /* 有 IP，最近一次同步成功（未配对时即有 IP） */
    COUPLE_NET_STATE_OFFLINE,      /* 有 IP，但最近一次同步失败 */
} couple_net_state_t;

/* ---------- 配对进度 ---------- */

typedef enum {
    COUPLE_NET_PAIR_NONE = 0,   /* 没有进行中的配对动作 */
    COUPLE_NET_PAIR_BEGIN,      /* 正在向 relay 申请配对码 */
    COUPLE_NET_PAIR_AWAITING,   /* 已拿到 6 位配对码，等对方在设备上用 UP/DOWN 录入 */
    COUPLE_NET_PAIR_PAIRED,     /* 对方已 claim，本机拿到配对三元组 */
    COUPLE_NET_PAIR_CLAIMED,    /* 本机输入了对方的配对码，claim 成功 */
    COUPLE_NET_PAIR_EXPIRED,    /* 配对码过期（本地按 TTL 判或被 relay 判定） */
    COUPLE_NET_PAIR_FAILED,     /* 其它失败，看 status */
    COUPLE_NET_PAIR_UNPAIRED,   /* 解绑流程走完（成功或本地放弃），看 status */
} couple_net_pair_phase_t;

typedef struct {
    couple_net_pair_phase_t phase;
    uint8_t status; /* couple_status_t 或 COUPLE_NET_STATUS_* */

    /* BEGIN / AWAITING 阶段有效：让 UI 把配对码念给用户听。 */
    char code[COUPLE_PAIR_CODE_LEN + 1u];
    uint16_t ttl_secs; /* BEGIN 时 relay 给的配对码有效期 */

    /* PAIRED / CLAIMED 阶段有效。共享密钥只写进 NVS，不从这里外传。 */
    uint8_t pair_id[COUPLE_PAIR_ID_LEN];
    uint8_t partner_id[COUPLE_DEVICE_ID_LEN];
    char partner_name[COUPLE_NAME_MAX + 1u];
} couple_net_pair_event_t;

/* ---------- 一次同步收到的内容 ---------- */

typedef struct {
    /* 对端与 relay 判定的服务端事件（含 OFFLINE_SUMMARY 之类本机不会发的类型）。 */
    const couple_event_list_t *events;
    bool partner_online;
    bool partner_holding; /* 对方此刻是否按着 OK */
    uint8_t pair_state;   /* couple_pair_state_t，PEER_LEFT 表示对方已解绑 */
} couple_net_sync_t;

/* ---------- 回调 ---------- */

/* 只在连接状态发生迁移时触发（start 后先按当前状态报一次）。 */
typedef void (*couple_net_state_cb_t)(couple_net_state_t state, void *user);

typedef void (*couple_net_sync_cb_t)(const couple_net_sync_t *sync, void *user);

typedef void (*couple_net_pair_cb_t)(const couple_net_pair_event_t *ev, void *user);

typedef struct {
    couple_net_state_cb_t on_state;
    couple_net_sync_cb_t on_sync;
    couple_net_pair_cb_t on_pair;
    void *user; /* 原样回传给上面三个回调 */
} couple_net_hooks_t;

/*
 * ============================ 公开接口 ============================
 *
 * 每个函数都标注了可以在哪个任务上下文调用：
 *   [任意任务]  任何任务都能调，内部自己保证线程安全
 *   [非 worker] 任何任务都能调，但绝不能在回调里调（回调就在 worker 里）
 */

/*
 * 启动 worker。hooks 里的函数指针可以为 NULL，但 hooks 本身不可为 NULL；
 * 内容会被复制一份，调用方之后不用保住它。
 * 同一时刻只允许一个实例：已经在跑时返回 ESP_ERR_INVALID_STATE。
 *
 * [任意任务，不能在回调里调]
 */
esp_err_t couple_net_start(const couple_net_hooks_t *hooks);

/*
 * 停掉 worker：等在途请求收尾、注销 Wi-Fi 回调、释放 HTTP 句柄。
 * 可以反复 start/stop，不泄漏任务与定时器。
 * 返回 ESP_ERR_TIMEOUT 表示 worker 没能在预期时间内退出（此时什么都没拆，
 * 稍后可以再调一次）。未启动时返回 ESP_OK。
 *
 * [非 worker]（在回调里调用会等自己而永久卡住）
 */
esp_err_t couple_net_stop(void);

/* 当前连接状态；未启动时返回 COUPLE_NET_STATE_IDLE。 [任意任务] */
couple_net_state_t couple_net_state(void);

/* 等价于 couple_net_state() == ONLINE。 [任意任务] */
bool couple_net_is_online(void);

/*
 * 上报「本机此刻正按着 OK」，会随下一次同步发给 relay。
 * 只置一个标志、掐断在飞的长轮询（一个非阻塞的 shutdown 调用）、投一条唤醒
 * 命令，都不阻塞，可以在按键回调里直接调。 [任意任务]
 */
void couple_net_set_holding(bool holding);

/*
 * 把一条互动事件排进 outbox（非阻塞，满则丢最旧并记警告）。
 *
 * 只接受本机可以发出的三类事件：MISS / HUG_START / HUG_END（value 按协议：
 * HUG_END 带 u16 时长；见 couple_types.h）。服务端事件（HUG_SYNC 等）由 relay
 * 下发，本机发出去会被 relay 整包拒掉，所以这里直接拒绝。
 *
 * 未启动、未配对时返回 ESP_ERR_INVALID_STATE：seq 水位属于某一段配对关系，
 * 配对之前入队会污染新 pair 的水位。
 *
 * [任意任务]
 */
esp_err_t couple_net_enqueue_event(const couple_event_t *ev);

/* outbox 里还压着几条事件（界面可以拿它显示「N 条没发出去」）。 [任意任务] */
uint8_t couple_net_outbox_count(void);

/* 当前 UTC 秒（最近一次 server_time + 单调流逝）；从未对过时返回 0。 [任意任务] */
uint32_t couple_net_clock_now_utc(void);

/*
 * 开始配对：向 relay 申请 6 位配对码，之后 worker 自己每约 2 秒轮询一次
 * /v1/pair/await，直到对方 claim 或配对码过期。
 * 进度全部通过 on_pair 回调上报（BEGIN -> AWAITING -> PAIRED/EXPIRED/FAILED）。
 * 已经在跑了、没 IP、没启动时返回错误；但「失败」也仍然会走 on_pair，
 * 调用方只要看回调和返回值任一即可。
 *
 * [非 worker]
 */
esp_err_t couple_net_pair_begin(void);

/*
 * 用对方给的 6 位配对码 claim（大小写不敏感，内部会转成大写）。
 * 成功走 on_pair(CLAIMED)，失败走 on_pair(FAILED) 并把协议状态码放进 status。
 * code 不必以 NUL 结尾，但 6 个字节都必须可读且非 NUL。
 *
 * 配对阶段的已知安全边界（设计取舍，不是遗漏）：
 *
 * relay 的 /v1/pair/claim 与 /v1/pair/await 响应在此时两端还没有共享密钥，
 * 因此它们无法验签 —— couple_proto_verify_frame() 以 key=NULL 调用，只做长度、
 * 魔数与类型校验（见 couple_net.c 里调用点旁的注释）。含义是：能猜到 6 位配对码
 * 的人、或链路上的中间人，有机会抢先替你把这段关系建立起来。
 *
 * 配对一旦完成，之后所有帧都走 HMAC-SHA256，上述窗口即关闭。当前威胁模型是
 * 「情侣自用的两块小设备 + 自己掌握的 relay」，因此按已知取舍接受；要收紧的话，
 * 正确做法是给配对再叠一条带外信道（例如屏幕上互认短数字，或二维码里带一次性
 * 公钥指纹），而不是在这里加签名。
 *
 * [非 worker]
 */
esp_err_t couple_net_pair_claim(const char code[COUPLE_PAIR_CODE_LEN]);

/*
 * 放弃进行中的配对：丢掉本地保存的配对码与 token，并停止 await 轮询。
 * 不报回调（调用方自己发起的，本来就知道），也不通知 relay
 * （配对码会自己过期）。
 *
 * [任意任务]
 */
void couple_net_pair_cancel(void);

/*
 * 解除配对：最多向 relay 发 3 次 /v1/unpair，然后无论结果如何都清掉本地配对
 * 与 outbox，并回调 on_pair(UNPAIRED)。这里的取舍是「用户意图优先」：
 * 用户已经点了解绑，就不能因为网络不好把本地状态留住、让两台设备继续显示成一对。
 * status 是最后一次拿到的状态（连不上时是 COUPLE_NET_STATUS_NETWORK）。
 *
 * [非 worker]
 */
esp_err_t couple_net_unpair(void);

/*
 * 告诉 worker「凭据 / relay 地址 / 名字变过了」：重新读一次 NVS，
 * 必要时重连 STA，并立刻跑一轮同步。配网完成、设置页保存之后调一次。
 *
 * [任意任务]
 */
esp_err_t couple_net_settings_changed(void);