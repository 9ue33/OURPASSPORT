/*
 * Heartlink 情侣心动终端 —— 网络 worker 实现（couple_net.h 的唯一实现）。
 *
 * ============================ 任务模型 ============================
 *
 * 只有一个 worker（"couple_net"，栈 NET_TASK_STACK、优先级 NET_TASK_PRIO）在跑：
 *
 *   任意任务                      worker 任务
 *   --------                      -----------
 *   enqueue_event()  -> outbox     取事件 -> 组帧 -> HTTP -> 验签 -> 解析 -> 回调
 *   set_holding()    -> 标志       pairing 状态机（begin / await / claim）
 *   pair_*()/unpair()-> 命令队列   unpair 重试
 *   settings_changed()-> 唤醒      Wi-Fi 事件驱动的重新求值
 *   clock_now_utc()  -> 临界区读
 *
 * 命令队列与退出信号量只创建一次、永不删除，所以 start/stop 可以反复调用而不泄漏。
 *
 * ============================ 回调约束 ============================
 *
 * 所有回调都在 worker 任务里同步执行，因此：
 *   - 回调里禁止阻塞（不 LVGL、不写 Flash、不做网络/音频、不打长日志）；
 *   - 回调里禁止调用 couple_net_stop()（会等自己退出而永久卡住），
 *     除 clock_now_utc()/state() 这类纯读外也尽量不要回调进来；
 *   - 回调参数里的指针只在本次回调内有效。
 *
 * ============================ 已知取舍 ============================
 *
 * 1) relay 对每个 /v1/sync 都先做长轮询（实测 8 秒），所以「刚入队的事件」本来
 *    最坏要等一轮才发得出去。现在按键/入队事件会调 net_poke_poll() 打断在飞的
 *    长轮询（shutdown 掉那个 socket），worker 立刻带着新事件重发，事件在毫秒级
 *    离开设备；被打断的这一轮不算失败、不涨退避、wait_ms 直接给 0。
 *
 *    代价（已知的耐久性缺口）：被打断的那次应答如果恰好携带了对方的事件，这些
 *    事件就丢了。relay 在写出报文之前就已经把 inbox 弹出，而同步请求里没有
 *    「已收到第几号服务端事件」的水位，本机事后无从补收。poke 只是把这个窗口从
 *    「最长 8 秒」缩到「relay 弹出之后到设备收到响应头之间」的毫秒级窗口，
 *    并没有消除它。
 *
 *    poke 只针对 /v1/sync：配对（begin / claim / await）与 unpair 的在途请求不能
 *    被腰斩，所以正在 await 配对时报到的新命令（例如 unpair）仍然要等一轮在途
 *    请求走完。
 *
 * 2) 只有 SYNC/UNPAIR 的响应带 HMAC。relay 在「这条配对不存在 / 不是成员」时
 *    会用全零密钥签一个错误帧（relay.py: do_POST 里 `if rsp_type in SIGNED_TYPES
 *    and key is None: key = ZERO_KEY`），本机用真实密钥必然验签失败。
 *    这种做法下「未认证的 status 字节」可以被任何人伪造，所以本模块只把它写进
 *    日志做诊断，绝不据它改状态：验签失败一律按网络故障处理并退避重试。
 *
 * 3) 只支持 http://。relay 地址写成 https:// 时本模块没有 TLS，必然连接失败，
 *    会退避重试并一直停在 OFFLINE（日志里只警告一次）。
 */

#include "couple_net.h"

#include "couple_config.h"
#include "couple_proto.h"
#include "couple_store.h"
#include "couple_wifi.h"

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>

static const char *TAG = "couple.net";

enum {
    NET_TASK_STACK = 6144,
    NET_TASK_PRIO = 5,
    NET_CMD_QUEUE_LEN = 8,
    NET_HTTP_BUF = 1024,       /* 响应头 + 一帧（<=542B）一次读得下 */
    NET_IDLE_POLL_MS = 1000,   /* 没 IP / 没在配对时的重新求值间隔 */
    NET_AWAIT_POLL_MS = 2000,  /* /v1/pair/await 的轮询间隔 */
    NET_UNPAIR_RETRY_MS = 500, /* 解绑重试的第一段间隔，之后翻倍 */
    NET_UNPAIR_TRIES = 3,
    NET_STOP_WAIT_MS = 35000,  /* > HTTP 超时，保证在途请求能收尾 */
    NET_TTL_FALLBACK_SEC = 300,
    NET_STA_RETRY_MS = 10000, /* STA 凭据下发失败后的重试间隔，别跟驱动的重连退避打架 */
    NET_URL_EXTRA = 24        /* s_url 里给 "/v1/pair/begin" 这类路径留的余量 */
};

typedef enum {
    NET_CMD_WAKE = 0,   /* 重新读 NVS / 重新求值（Wi-Fi 事件、设置变更） */
    NET_CMD_PAIR_BEGIN, /* 申请配对码 */
    NET_CMD_PAIR_CLAIM, /* 用对方的配对码 claim，code 已转大写 */
    NET_CMD_PAIR_CANCEL,/* 放弃进行中的配对，静默清本地会话 */
    NET_CMD_UNPAIR,     /* 解绑 */
    NET_CMD_KICK,       /* 唤醒 worker：新事件/状态刚入队，立刻重发一轮 */
    NET_CMD_STOP        /* 退出 worker */
} net_cmd_kind_t;

typedef struct {
    uint8_t kind; /* net_cmd_kind_t */
    char code[COUPLE_PAIR_CODE_LEN + 2u];
} net_cmd_t;

typedef enum {
    NET_PAIR_REQ_NONE = 0,
    NET_PAIR_REQ_BEGIN,
    NET_PAIR_REQ_CLAIM
} net_pair_req_t;

typedef struct {
    uint32_t seq;
    couple_event_t ev;
} net_outbox_item_t;

/* ---------- 单例状态 ---------- */

static QueueHandle_t s_cmd_queue;
static SemaphoreHandle_t s_exit_sem;
static TaskHandle_t s_task;
static couple_net_hooks_t s_hooks;

/*
 * 在飞的 /v1/sync 长轮询的 socket；-1 表示当前没有可打断的轮询。
 * 只在收到响应头之前有效，见 net_http_post()。
 */
static volatile int s_http_fd = -1;
/* 轮询是被我们自己打断的（不是网络故障），用来区分日志与退避。 */
static volatile bool s_http_poked;

static volatile bool s_started;
static volatile uint8_t s_state = (uint8_t)COUPLE_NET_STATE_IDLE; /* couple_net_state_t */
static uint8_t s_state_reported = 0xFFu; /* 0xFF = 还没报过，用来实现「变化才报」 */

static bool s_have_ip;
static bool s_sync_failed;

static bool s_sta_cfg_valid; /* 是不是已经把 s_sta_ssid 的凭据下发过且成功 */
static char s_sta_ssid[COUPLE_WIFI_SSID_MAX];
static int64_t s_sta_retry_at_us;

static bool s_holding;      /* 本机此刻按着 OK（调用方写、worker 读） */
static bool s_holding_sent; /* 上一次成功同步时上报的值 */

static volatile bool s_unpair_req;
static net_pair_req_t s_pair_req;
static char s_pair_claim_code[COUPLE_PAIR_CODE_LEN + 1u];

/* 进行中的配对会话：begin 成功后进入 await 阶段，只用 worker 自己的数据 */
static bool s_pair_active;
static char s_pair_code[COUPLE_PAIR_CODE_LEN + 1u];
static uint8_t s_pair_token[COUPLE_PAIR_TOKEN_LEN];
static uint16_t s_pair_ttl_secs;
static int64_t s_pair_deadline_us;

/* HTTP 客户端按 base 复用；base 变了才重建。每轮用完 close()，不开 keep-alive。 */
static esp_http_client_handle_t s_http;
static char s_http_base[COUPLE_RELAY_URL_MAX];
static bool s_warned_no_url;
static bool s_warned_https;

static net_outbox_item_t s_outbox[COUPLE_OUTBOX_CAPACITY];
static uint8_t s_outbox_head;
static uint8_t s_outbox_count;
static portMUX_TYPE s_outbox_lock = portMUX_INITIALIZER_UNLOCKED;

/* 对时：最近的 server_time + 之后单调流逝的时间 */
static portMUX_TYPE s_clock_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_clock_utc;
static int64_t s_clock_base_us;
static bool s_clock_synced;

/* worker 私有缓冲（放 static 省栈；只有 worker 任务碰它们） */
static uint8_t s_frame[COUPLE_PROTO_MAX_FRAME];
static uint8_t s_rsp[COUPLE_PROTO_MAX_FRAME];
static char s_url[COUPLE_RELAY_URL_MAX + NET_URL_EXTRA];
static char s_hex[COUPLE_PAIR_TOKEN_LEN * 2u + 1u];

/* ---------- 小工具 ---------- */

/* 手写替代 strncpy：永远 NUL 结尾，src 为 NULL 时写空串。 */
static void net_copy_str(char *out, size_t cap, const char *src)
{
    size_t i = 0u;

    if (out == NULL || cap == 0u) {
        return;
    }
    if (src != NULL) {
        while (i + 1u < cap && src[i] != '\0') {
            out[i] = src[i];
            i++;
        }
    }
    out[i] = '\0';
}

/* 手写 hex（小写，relay 的 _hex_header 会自己 strip+lower）。cap 不够时返回 0。 */
static size_t net_hex(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char kHex[] = "0123456789abcdef";
    size_t i;

    if (in == NULL || out == NULL || cap < len * 2u + 1u) {
        if (out != NULL && cap > 0u) {
            out[0] = '\0';
        }
        return 0u;
    }
    for (i = 0u; i < len; i++) {
        out[i * 2u] = kHex[(in[i] >> 4) & 0x0Fu];
        out[i * 2u + 1u] = kHex[in[i] & 0x0Fu];
    }
    out[len * 2u] = '\0';
    return len * 2u;
}

static void net_warn_once(bool *flag, const char *msg)
{
    if (*flag) {
        return;
    }
    *flag = true;
    ESP_LOGW(TAG, "%s", msg);
}

/* base 尾部斜杠会被裁掉，拼成 base + path。 */
static bool net_build_url(char *out, size_t cap, const char *base, const char *path)
{
    size_t base_len;
    size_t path_len;
    size_t i;

    if (out == NULL || cap == 0u || base == NULL || path == NULL) {
        return false;
    }
    base_len = strlen(base);
    while (base_len > 0u && base[base_len - 1u] == '/') {
        base_len--;
    }
    path_len = strlen(path);
    if (base_len + path_len + 1u > cap) {
        out[0] = '\0';
        return false;
    }
    for (i = 0u; i < base_len; i++) {
        out[i] = base[i];
    }
    for (i = 0u; i < path_len; i++) {
        out[base_len + i] = path[i];
    }
    out[base_len + path_len] = '\0';
    return true;
}

/* 只认「恰好 1 字节」的裸 status（relay 对多数错误只回一个字节）。 */
static bool net_bare_status(const uint8_t *payload, size_t len, uint8_t *out)
{
    if (payload == NULL || out == NULL || len != 1u) {
        return false;
    }
    *out = payload[0];
    return true;
}

/*
 * 验签失败时的诊断。读出的 status 未经认证，只能打日志。
 */
static void net_log_bad_frame(const uint8_t *buf, size_t len, const char *what)
{
    couple_frame_hdr_t hdr;

    if (len >= (size_t)COUPLE_PROTO_HDR_LEN && couple_proto_decode_header(buf, len, &hdr)) {
        /* 0x02/0x04/0x06/0x08/0x0A 都是响应帧，首字节按协议是 status。 */
        const bool is_rsp = ((hdr.type & 1u) == 0u) && hdr.type >= (uint8_t)COUPLE_FRAME_PAIR_BEGIN_RSP;
        if (is_rsp && hdr.payload_len >= 1u && len >= (size_t)COUPLE_PROTO_HDR_LEN + 1u) {
            ESP_LOGW(TAG, "%s响应验签失败（未认证 status=%u，仅作诊断：relay 可能已经不认得这条配对了）",
                     what, (unsigned)buf[COUPLE_PROTO_HDR_LEN]);
            return;
        }
    }
    ESP_LOGW(TAG, "%s响应不是合法的签名帧", what);
}

/* ---------- outbox ---------- */

static void net_outbox_clear(void)
{
    portENTER_CRITICAL(&s_outbox_lock);
    s_outbox_head = 0u;
    s_outbox_count = 0u;
    portEXIT_CRITICAL(&s_outbox_lock);
}

/* 丢掉所有 seq <= ack 的条目（relay 的 ack 就是它记住的 last_accepted）。 */
static void net_outbox_prune(uint32_t ack)
{
    portENTER_CRITICAL(&s_outbox_lock);
    while (s_outbox_count > 0u && s_outbox[s_outbox_head].seq <= ack) {
        s_outbox_head = (uint8_t)((s_outbox_head + 1u) % (uint8_t)COUPLE_OUTBOX_CAPACITY);
        s_outbox_count--;
    }
    portEXIT_CRITICAL(&s_outbox_lock);
}

/*
 * 清空 outbox，并在真的丢了东西时留一条日志。
 * 这些事件已经没有可能再发出去，静默丢弃过的东西事后无从追查。
 */
static void net_outbox_drop_all(const char *why)
{
    uint8_t n;

    portENTER_CRITICAL(&s_outbox_lock);
    n = s_outbox_count;
    portEXIT_CRITICAL(&s_outbox_lock);

    if (n > 0u) {
        ESP_LOGW(TAG, "%s：丢弃 outbox 中 %u 条未确认事件", why, (unsigned)n);
    }
    net_outbox_clear();
}

/* ---------- 时间 ---------- */

static void net_set_clock(uint32_t utc)
{
    if (utc == 0u) {
        return;
    }
    const int64_t now_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_clock_lock);
    s_clock_utc = utc;
    s_clock_base_us = now_us;
    s_clock_synced = true;
    portEXIT_CRITICAL(&s_clock_lock);
}

/* ---------- 组帧与收发 ---------- */

/* payload 要事先写在 s_frame + COUPLE_PROTO_HDR_LEN；返回帧总长度。 */
static size_t net_build_frame(uint8_t type, uint32_t seq, uint32_t ack, size_t payload_len,
                              const uint8_t key[COUPLE_PROTO_KEY_LEN])
{
    if (couple_proto_encode_header(s_frame, sizeof(s_frame), type, (uint16_t)payload_len, seq, ack) == 0u) {
        return 0u;
    }
    if (key == NULL) {
        /* 配对握手帧不带 HMAC。 */
        return (size_t)COUPLE_PROTO_HDR_LEN + payload_len;
    }
    return couple_proto_finalize(s_frame, type, (uint16_t)payload_len, key);
}

/*
 * 打断在飞的长轮询，让 worker 立刻带着新事件重发。
 * 只针对 /v1/sync：配对（begin/claim/await）和 unpair 的请求不能被腰斩，
 * 而 worker 除了这几件事不做别的。
 * 由 UI / 按键回调调用，所以必须非阻塞——对 lwip socket 调 shutdown() 就是。
 */
static void net_poke_poll(void)
{
    int fd = s_http_fd;

    if (fd < 0) {
        return;
    }
    s_http_poked = true;
    shutdown(fd, SHUT_RDWR);
}

/*
 * POST 一帧，成功时把响应读进 s_rsp，返回读到的字节数（<=0 表示失败）。
 * pair_id / pair_token 传 NULL 表示不带对应的头。
 */
static int net_http_post(const couple_store_blob_t *snap, const char *path, const uint8_t *body,
                         size_t body_len, const uint8_t *pair_id, const uint8_t *pair_token)
{
    char base[COUPLE_RELAY_URL_MAX];

    net_copy_str(base, sizeof(base), snap->relay_url);
    if (base[0] == '\0') {
        net_warn_once(&s_warned_no_url, "relay 地址为空，先完成配网再同步");
        return -1;
    }
    if (strncmp(base, "https://", 8u) == 0) {
        net_warn_once(&s_warned_https, "relay 地址是 https://，本模块不做 TLS，连接会一直失败");
    }
    if (!net_build_url(s_url, sizeof(s_url), base, path)) {
        ESP_LOGE(TAG, "URL 太长，拼不出来: %s%s", base, path);
        return -1;
    }

    if (s_http == NULL || strcmp(s_http_base, base) != 0) {
        if (s_http != NULL) {
            esp_http_client_cleanup(s_http);
            s_http = NULL;
            s_http_base[0] = '\0';
        }
        const esp_http_client_config_t cfg = {
            .url = s_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = COUPLE_HTTP_TIMEOUT_MS,
            .buffer_size = NET_HTTP_BUF,
        };
        s_http = esp_http_client_init(&cfg);
        if (s_http == NULL) {
            ESP_LOGE(TAG, "HTTP 客户端创建失败: %s", s_url);
            return -1;
        }
        net_copy_str(s_http_base, sizeof(s_http_base), base);
    }

    esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    esp_http_client_set_url(s_http, s_url);
    esp_http_client_set_header(s_http, "Content-Type", "application/octet-stream");

    if (net_hex(snap->device_id, COUPLE_DEVICE_ID_LEN, s_hex, sizeof(s_hex)) > 0u) {
        esp_http_client_set_header(s_http, "X-Device-Id", s_hex);
    }
    if (pair_id != NULL) {
        (void)net_hex(pair_id, COUPLE_PAIR_ID_LEN, s_hex, sizeof(s_hex));
        esp_http_client_set_header(s_http, "X-Pair-Id", s_hex);
    } else {
        (void)esp_http_client_delete_header(s_http, "X-Pair-Id");
    }
    if (pair_token != NULL) {
        (void)net_hex(pair_token, COUPLE_PAIR_TOKEN_LEN, s_hex, sizeof(s_hex));
        esp_http_client_set_header(s_http, "X-Pair-Token", s_hex);
    } else {
        (void)esp_http_client_delete_header(s_http, "X-Pair-Token");
    }

    if (esp_http_client_open(s_http, (int)body_len) != ESP_OK) {
        ESP_LOGW(TAG, "连接 relay 失败: %s", s_url);
        esp_http_client_close(s_http);
        return -1;
    }

    /*
     * 只有 /v1/sync 允许被 net_poke_poll() 打断，所以只有它发布 fd。
     * 发布时顺手清掉上一轮可能残留的 s_http_poked：这个标志描述的是「当前在飞的这
     * 一轮被打断过」，上一轮的记录不能拿来解释这一轮的失败。
     */
    const bool pokeable = (strcmp(path, "/v1/sync") == 0);
    if (pokeable) {
        s_http_poked = false;
        s_http_fd = esp_http_client_get_socket(s_http);
    }

    /* 没有 tx buffer 时 write 直接落到 socket，可能只写一半，循环写完。 */
    size_t written = 0u;
    while (written < body_len) {
        const int n = esp_http_client_write(s_http, (const char *)body + written,
                                            (int)(body_len - written));
        if (n <= 0) {
            break;
        }
        written += (size_t)n;
    }
    if (written != body_len) {
        ESP_LOGW(TAG, "请求体发送不完整 (%u/%u 字节)", (unsigned)written, (unsigned)body_len);
        if (pokeable) {
            s_http_fd = -1;
        }
        esp_http_client_close(s_http);
        return -1;
    }

    const int hdr_ret = esp_http_client_fetch_headers(s_http);
    /*
     * 立刻撤回 fd：relay 在写出响应头之前就已经把对方的事件从 inbox 里弹出了，
     * 所以「发布 fd」到「收到响应头」这段时间里，poke 能打断的只可能是空轮询；
     * 过了这个点再 poke 只会把已经在线上的字节丢掉。
     */
    if (pokeable) {
        s_http_fd = -1;
    }
    if (hdr_ret < 0) {
        ESP_LOGW(TAG, "读取响应头失败: %s", s_url);
        esp_http_client_close(s_http);
        return -1;
    }
    const int code = esp_http_client_get_status_code(s_http);
    if (code != 200) {
        ESP_LOGW(TAG, "relay 返回 HTTP %d: %s", code, s_url);
        esp_http_client_close(s_http);
        return -1;
    }

    /*
     * relay 一定带 Content-Length + Connection: close，所以「读到 read() 返回 <=0 为止」
     * 比 esp_http_client_is_complete_data_received() 更可靠（后者在连接被关掉时可能
     * 因为 content_length 对不上而误报 false）。
     */
    size_t got = 0u;
    while (got < sizeof(s_rsp)) {
        const int n = esp_http_client_read(s_http, (char *)s_rsp + got, (int)(sizeof(s_rsp) - got));
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    esp_http_client_close(s_http);

    if (got == 0u) {
        ESP_LOGW(TAG, "relay 没有返回内容: %s", s_url);
        return -1;
    }
    return (int)got;
}

/* ---------- 退避 ---------- */

/* 指数退避 + 最多 25% 抖动；把下一档写回 retry_ms。 */
static uint32_t net_backoff(uint32_t *retry_ms)
{
    const uint32_t base = *retry_ms;
    const uint32_t span = (base / 4u) + 1u;
    const uint32_t delay = base + (esp_random() % span);
    const uint32_t next = base * 2u;

    *retry_ms = (next > (uint32_t)COUPLE_RETRY_MAX_MS) ? (uint32_t)COUPLE_RETRY_MAX_MS : next;
    return delay;
}

/* ---------- Wi-Fi / 状态 ---------- */

static void net_apply_wifi(const couple_store_blob_t *snap)
{
    const bool has_wifi = couple_store_has_wifi();
    const bool same_target = (strcmp(s_sta_ssid, snap->wifi_ssid) == 0);

    if (!has_wifi) {
        if (s_sta_cfg_valid) {
            couple_wifi_sta_stop();
            s_sta_cfg_valid = false;
            s_sta_ssid[0] = '\0';
            ESP_LOGI(TAG, "Wi-Fi 凭据已清除，停用 STA");
        }
    } else {
        /*
         * 只在「换了 SSID」或「上次下发失败且已过重试间隔」时重新下发，
         * 免得每秒重配置一次、把 couple_wifi 自己的重连退避一直顶回去。
         */
        const int64_t now_us = esp_timer_get_time();
        const bool changed = !same_target;
        const bool retry = !s_sta_cfg_valid && now_us >= s_sta_retry_at_us;

        if (changed || retry) {
            const esp_err_t err = couple_wifi_sta_start(snap->wifi_ssid, snap->wifi_pass);
            const bool ok = (err == ESP_OK);
            /* 只在结果或目标发生变化时打日志，避免 1Hz 刷屏。 */
            const bool report = changed || (s_sta_cfg_valid != ok);

            net_copy_str(s_sta_ssid, sizeof(s_sta_ssid), snap->wifi_ssid);
            s_sta_cfg_valid = ok;
            s_sta_retry_at_us = now_us + (int64_t)NET_STA_RETRY_MS * 1000;

            if (report) {
                if (ok) {
                    ESP_LOGI(TAG, "STA 凭据已下发: %s", s_sta_ssid);
                } else {
                    ESP_LOGW(TAG, "STA 启动失败: %s", esp_err_to_name(err));
                }
            }
        }
    }

    s_have_ip = couple_wifi_sta_has_ip();
}

/* 未配对时「有 IP 就算在线」；已配对时看最近一轮同步的结果。 */
static couple_net_state_t net_current_state(void)
{
    if (!couple_store_has_wifi()) {
        return COUPLE_NET_STATE_IDLE;
    }
    if (!s_have_ip) {
        return COUPLE_NET_STATE_WIFI_WAIT;
    }
    if (!couple_store_is_paired() || !s_sync_failed) {
        return COUPLE_NET_STATE_ONLINE;
    }
    return COUPLE_NET_STATE_OFFLINE;
}

static void net_report_state(void)
{
    const couple_net_state_t st = net_current_state();

    if ((uint8_t)st == s_state_reported) {
        return;
    }
    s_state_reported = (uint8_t)st;
    s_state = (uint8_t)st;
    if (s_hooks.on_state != NULL) {
        s_hooks.on_state(st, s_hooks.user);
    }
}

/*
 * Wi-Fi 回调跑在默认事件循环任务里：只允许入队这种瞬时动作。
 * 具体动作（切 STA、重连、跑一轮同步）全部留给 worker。
 */
static void net_wifi_event_cb(couple_wifi_event_t ev, uint8_t reason, void *user)
{
    (void)ev;
    (void)reason;
    (void)user;

    if (!s_started || s_cmd_queue == NULL) {
        return;
    }
    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = (uint8_t)NET_CMD_WAKE;
    (void)xQueueSend(s_cmd_queue, &cmd, 0);
}

/* ---------- 命令队列 ---------- */

static void net_pair_session_clear(void)
{
    s_pair_active = false;
    s_pair_code[0] = '\0';
    s_pair_ttl_secs = 0u;
    s_pair_deadline_us = 0;
    memset(s_pair_token, 0, sizeof(s_pair_token));
}

/* 返回 false 表示收到 STOP，worker 应当退出。 */
static bool net_handle_cmd(const net_cmd_t *cmd)
{
    switch (cmd->kind) {
    case NET_CMD_WAKE:
        break;
    case NET_CMD_PAIR_BEGIN:
        s_pair_req = NET_PAIR_REQ_BEGIN;
        break;
    case NET_CMD_PAIR_CLAIM:
        net_copy_str(s_pair_claim_code, sizeof(s_pair_claim_code), cmd->code);
        s_pair_req = NET_PAIR_REQ_CLAIM;
        break;
    case NET_CMD_PAIR_CANCEL:
        /* 调用方自己发起的中止，按契约不报回调。 */
        net_pair_session_clear();
        s_pair_req = NET_PAIR_REQ_NONE;
        break;
    case NET_CMD_UNPAIR:
        s_unpair_req = true;
        break;
    case NET_CMD_KICK:
        break; /* 只是唤醒停在 net_wait_cmd() 上的 worker，同步本身不需要动作 */
    case NET_CMD_STOP:
    default:
        return false;
    }
    return true;
}

/* 排空队列里已到的命令。 */
static bool net_pump_commands(void)
{
    net_cmd_t cmd;

    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE) {
        if (!net_handle_cmd(&cmd)) {
            return false;
        }
    }
    return true;
}

/*
 * 既当退避睡眠，又能被命令立刻打断（ms 为 0 时退化成排空一次队列）。
 */
static bool net_wait_cmd(uint32_t ms)
{
    net_cmd_t cmd;

    if (ms == 0u) {
        return net_pump_commands();
    }
    if (xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(ms)) == pdTRUE) {
        if (!net_handle_cmd(&cmd)) {
            return false;
        }
    }
    return net_pump_commands();
}

static esp_err_t net_post_cmd(net_cmd_kind_t kind, const char *code)
{
    net_cmd_t cmd;

    if (!s_started || s_cmd_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = (uint8_t)kind;
    if (code != NULL) {
        net_copy_str(cmd.code, sizeof(cmd.code), code);
    }
    return (xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE) ? ESP_OK : ESP_ERR_TIMEOUT;
}

/* ---------- 配对回调 ---------- */

/*
 * 上报配对进度。code/ttl 只在 BEGIN/AWAITING 有意义（BEGIN 时还没拿到码）；
 * PAIRED/CLAIMED 的身份信息从 store 现读一份（set_pair 已经写好了）。
 */
static void net_emit_pair(couple_net_pair_phase_t phase, uint8_t status)
{
    couple_net_pair_event_t ev;

    if (s_hooks.on_pair == NULL) {
        return;
    }
    memset(&ev, 0, sizeof(ev));
    ev.phase = phase;
    ev.status = status;

    if (phase == COUPLE_NET_PAIR_BEGIN || phase == COUPLE_NET_PAIR_AWAITING) {
        net_copy_str(ev.code, sizeof(ev.code), s_pair_code);
        ev.ttl_secs = s_pair_ttl_secs;
    } else if (phase == COUPLE_NET_PAIR_PAIRED || phase == COUPLE_NET_PAIR_CLAIMED) {
        couple_store_blob_t cur;
        couple_store_snapshot(&cur);
        memcpy(ev.pair_id, cur.pair_id, sizeof(ev.pair_id));
        memcpy(ev.partner_id, cur.partner_id, sizeof(ev.partner_id));
        net_copy_str(ev.partner_name, sizeof(ev.partner_name), cur.partner_name);
    }

    s_hooks.on_pair(&ev, s_hooks.user);
}

/* ---------- 配对流程 ---------- */

static void net_do_pair_begin(const couple_store_blob_t *snap)
{
    net_pair_session_clear();
    net_emit_pair(COUPLE_NET_PAIR_BEGIN, COUPLE_STATUS_OK);

    const size_t plen = couple_proto_encode_pair_begin_req(s_frame + COUPLE_PROTO_HDR_LEN,
                                                           sizeof(s_frame) - COUPLE_PROTO_HDR_LEN,
                                                           snap->self_name);
    if (plen == 0u) {
        ESP_LOGE(TAG, "配对请求编码失败（名字太长？）");
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_LOCAL);
        return;
    }
    /* 握手帧不带 HMAC，也没有 seq 语义（relay 的 handle_begin 不看 seq）。 */
    const size_t flen = net_build_frame(COUPLE_FRAME_PAIR_BEGIN_REQ, 0u, 0u, plen, NULL);
    if (flen == 0u) {
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_LOCAL);
        return;
    }

    const int got = net_http_post(snap, "/v1/pair/begin", s_frame, flen, NULL, NULL);
    if (got <= 0) {
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        return;
    }

    couple_frame_hdr_t hdr;
    if (!couple_proto_verify_frame(s_rsp, (size_t)got, NULL, &hdr) ||
        hdr.type != (uint8_t)COUPLE_FRAME_PAIR_BEGIN_RSP) {
        net_log_bad_frame(s_rsp, (size_t)got, "配对申请");
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        return;
    }

    couple_pair_begin_rsp_t rsp;
    if (!couple_proto_decode_pair_begin_rsp(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &rsp)) {
        uint8_t st = 0u;
        if (net_bare_status(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &st)) {
            net_emit_pair(COUPLE_NET_PAIR_FAILED, st);
        } else {
            net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        }
        return;
    }
    if (rsp.status != (uint8_t)COUPLE_STATUS_OK) {
        ESP_LOGW(TAG, "申请配对码被拒: status=%u", (unsigned)rsp.status);
        net_emit_pair(COUPLE_NET_PAIR_FAILED, rsp.status);
        return;
    }

    s_pair_active = true;
    net_copy_str(s_pair_code, sizeof(s_pair_code), rsp.code);
    memcpy(s_pair_token, rsp.token, sizeof(s_pair_token));
    s_pair_ttl_secs = (rsp.ttl_secs != 0u) ? rsp.ttl_secs : (uint16_t)NET_TTL_FALLBACK_SEC;
    s_pair_deadline_us = esp_timer_get_time() + (int64_t)s_pair_ttl_secs * 1000000;

    ESP_LOGI(TAG, "配对码 %s，有效期 %u 秒，开始轮询对方是否 claim",
             s_pair_code, (unsigned)s_pair_ttl_secs);
    net_emit_pair(COUPLE_NET_PAIR_AWAITING, COUPLE_STATUS_OK);
}

static void net_do_pair_claim(const couple_store_blob_t *snap)
{
    const size_t plen = couple_proto_encode_pair_claim_req(s_frame + COUPLE_PROTO_HDR_LEN,
                                                           sizeof(s_frame) - COUPLE_PROTO_HDR_LEN,
                                                           s_pair_claim_code, snap->self_name);
    if (plen == 0u) {
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_LOCAL);
        return;
    }
    const size_t flen = net_build_frame(COUPLE_FRAME_PAIR_CLAIM_REQ, 0u, 0u, plen, NULL);
    if (flen == 0u) {
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_LOCAL);
        return;
    }

    const int got = net_http_post(snap, "/v1/pair/claim", s_frame, flen, NULL, NULL);
    if (got <= 0) {
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        return;
    }

    couple_frame_hdr_t hdr;
    if (!couple_proto_verify_frame(s_rsp, (size_t)got, NULL, &hdr) ||
        hdr.type != (uint8_t)COUPLE_FRAME_PAIR_CLAIM_RSP) {
        net_log_bad_frame(s_rsp, (size_t)got, "claim");
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        return;
    }

    couple_pair_result_t rsp;
    if (!couple_proto_decode_pair_claim_rsp(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &rsp)) {
        /* relay 失败时只回 1 字节 status，不按完整结构编码。 */
        uint8_t st = 0u;
        if (net_bare_status(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &st)) {
            ESP_LOGW(TAG, "claim 被拒: status=%u", (unsigned)st);
            net_emit_pair(COUPLE_NET_PAIR_FAILED, st);
        } else {
            net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_NETWORK);
        }
        return;
    }
    if (rsp.status != (uint8_t)COUPLE_STATUS_OK) {
        ESP_LOGW(TAG, "claim 被拒: status=%u", (unsigned)rsp.status);
        net_emit_pair(COUPLE_NET_PAIR_FAILED, rsp.status);
        return;
    }

    const esp_err_t err = couple_store_set_pair(rsp.pair_id, rsp.key, rsp.partner_id, rsp.partner_name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配对结果写入 NVS 失败: %s", esp_err_to_name(err));
        net_emit_pair(COUPLE_NET_PAIR_FAILED, COUPLE_NET_STATUS_LOCAL);
        return;
    }

    /*
     * 配对成功即换了伙伴：outbox 里可能还压着上一段关系没被 ack 的事件，
     * 它们带着旧 seq，绝不能发到新伙伴那里。
     */
    net_outbox_drop_all("重新配对");
    net_pair_session_clear();
    s_holding_sent = s_holding;
    ESP_LOGI(TAG, "配对完成（对方输入了本机申请过/对方申请的码）");
    net_emit_pair(COUPLE_NET_PAIR_CLAIMED, COUPLE_STATUS_OK);
}

/*
 * 跑一轮 /v1/pair/await。返回 ESP_OK 表示还在等（下一轮按 NET_AWAIT_POLL_MS 再poll）；
 * 其它值表示这一轮配对流程已经结束或本轮失败，调用方应退避。
 */
static esp_err_t net_await_step(const couple_store_blob_t *snap)
{
    if (!s_pair_active) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 本地 TTL 判定：relay 失联时别把配对码永远轮询下去。 */
    if (esp_timer_get_time() >= s_pair_deadline_us) {
        ESP_LOGW(TAG, "配对码已过期（本地 TTL %u 秒）", (unsigned)s_pair_ttl_secs);
        net_pair_session_clear();
        net_emit_pair(COUPLE_NET_PAIR_EXPIRED, COUPLE_STATUS_CODE_EXPIRED);
        return ESP_ERR_TIMEOUT;
    }

    /* 空载荷：encode 返回 0 是正常的。 */
    (void)couple_proto_encode_pair_await_req(s_frame + COUPLE_PROTO_HDR_LEN,
                                             sizeof(s_frame) - COUPLE_PROTO_HDR_LEN);
    const size_t flen = net_build_frame(COUPLE_FRAME_PAIR_AWAIT_REQ, 0u, 0u, 0u, NULL);
    if (flen == 0u) {
        return ESP_FAIL;
    }

    /*
     * 这里用完整的 HTTP 超时（> relay 自己的 25 秒长轮询），
     * 绝不中途放弃一个已经在途的请求。
     */
    const int got = net_http_post(snap, "/v1/pair/await", s_frame, flen, NULL, s_pair_token);
    if (got <= 0) {
        return ESP_FAIL;
    }

    couple_frame_hdr_t hdr;
    if (!couple_proto_verify_frame(s_rsp, (size_t)got, NULL, &hdr) ||
        hdr.type != (uint8_t)COUPLE_FRAME_PAIR_AWAIT_RSP) {
        net_log_bad_frame(s_rsp, (size_t)got, "await");
        return ESP_FAIL;
    }

    couple_pair_await_rsp_t rsp;
    if (!couple_proto_decode_pair_await_rsp(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &rsp)) {
        uint8_t st = 0u;
        if (net_bare_status(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &st) &&
            st == (uint8_t)COUPLE_STATUS_UNAUTHORIZED) {
            /* token 不匹配：这个配对码已经不归本机了，别再问下去。 */
            ESP_LOGW(TAG, "配对 token 被拒，放弃配对");
            net_pair_session_clear();
            net_emit_pair(COUPLE_NET_PAIR_FAILED, st);
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_FAIL;
    }

    if (rsp.status != (uint8_t)COUPLE_STATUS_OK) {
        ESP_LOGW(TAG, "await 被拒: status=%u", (unsigned)rsp.status);
        net_pair_session_clear();
        net_emit_pair(COUPLE_NET_PAIR_FAILED, rsp.status);
        return ESP_ERR_INVALID_STATE;
    }

    if (rsp.state == (uint8_t)COUPLE_PAIR_AWAIT_PAIRED) {
        if (!couple_pair_await_has_identity(&rsp)) {
            /* 服务端还没把三元组给全，保守地再等一轮。 */
            ESP_LOGW(TAG, "await 说配好了但身份字段不全，再等一轮");
            return ESP_FAIL;
        }
        const esp_err_t err = couple_store_set_pair(rsp.pair_id, rsp.key, rsp.partner_id, rsp.partner_name);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "配对结果写入 NVS 失败: %s", esp_err_to_name(err));
            return ESP_FAIL;
        }
        /* 同上：换伙伴前丢掉上一段关系遗留的事件。 */
        net_outbox_drop_all("重新配对");
        net_pair_session_clear();
        s_holding_sent = s_holding;
        ESP_LOGI(TAG, "对方已 claim，配对完成");
        net_emit_pair(COUPLE_NET_PAIR_PAIRED, COUPLE_STATUS_OK);
        return ESP_ERR_INVALID_STATE;
    }

    if (rsp.state == (uint8_t)COUPLE_PAIR_AWAIT_EXPIRED) {
        net_pair_session_clear();
        net_emit_pair(COUPLE_NET_PAIR_EXPIRED, COUPLE_STATUS_CODE_EXPIRED);
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK; /* WAITING */
}

/* ---------- 解绑 ---------- */

static uint8_t net_unpair_try(const couple_store_blob_t *snap)
{
    const uint32_t seq = couple_store_bump_seq();

    (void)couple_proto_encode_unpair_req(s_frame + COUPLE_PROTO_HDR_LEN,
                                         sizeof(s_frame) - COUPLE_PROTO_HDR_LEN);
    const size_t flen = net_build_frame(COUPLE_FRAME_UNPAIR_REQ, seq, snap->sync_ack, 0u, snap->pair_key);
    if (flen == 0u) {
        return COUPLE_NET_STATUS_LOCAL;
    }

    const int got = net_http_post(snap, "/v1/unpair", s_frame, flen, snap->pair_id, NULL);
    if (got <= 0) {
        return COUPLE_NET_STATUS_NETWORK;
    }

    couple_frame_hdr_t hdr;
    if (!couple_proto_verify_frame(s_rsp, (size_t)got, snap->pair_key, &hdr) ||
        hdr.type != (uint8_t)COUPLE_FRAME_UNPAIR_RSP) {
        net_log_bad_frame(s_rsp, (size_t)got, "解绑");
        return COUPLE_NET_STATUS_NETWORK;
    }

    uint8_t st = COUPLE_NET_STATUS_NETWORK;
    if (!couple_proto_decode_unpair_rsp(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &st)) {
        return COUPLE_NET_STATUS_NETWORK;
    }
    return st;
}

/* 返回 false 表示处理途中收到 STOP，worker 应当直接退出。 */
static bool net_do_unpair(const couple_store_blob_t *snap)
{
    uint8_t status = COUPLE_NET_STATUS_NETWORK;

    if (!couple_store_is_paired()) {
        status = COUPLE_STATUS_OK;
    } else if (!s_have_ip) {
        /* 用户意图优先：连不上也照样把本地状态清干净。 */
        ESP_LOGW(TAG, "没有 IP，解绑只改本地状态");
    } else {
        uint32_t delay_ms = NET_UNPAIR_RETRY_MS;
        uint8_t i;

        for (i = 0u; i < (uint8_t)NET_UNPAIR_TRIES; i++) {
            status = net_unpair_try(snap);
            if (status == (uint8_t)COUPLE_STATUS_OK) {
                break;
            }
            if (i + 1u < (uint8_t)NET_UNPAIR_TRIES) {
                if (!net_wait_cmd(delay_ms)) {
                    return false;
                }
                delay_ms *= 2u;
            }
        }
    }

    (void)couple_store_clear_pair();
    net_outbox_drop_all("解绑");
    net_pair_session_clear();
    s_pair_req = NET_PAIR_REQ_NONE;
    s_holding_sent = s_holding;
    ESP_LOGI(TAG, "本地配对已清除 (relay status=%u)", (unsigned)status);
    net_emit_pair(COUPLE_NET_PAIR_UNPAIRED, status);
    return true;
}

/* ---------- 同步 ---------- */

/* relay 是 Love Energy 的唯一权威：每次同步都把本机值覆盖成 pair 的总数，
 * 两块设备因此永远显示同一个数字。回调极短（只比一个数）。 */
static void net_apply_relay_energy(couple_stats_t *stats, void *user)
{
    const uint32_t *value = (const uint32_t *)user;

    if (stats->energy != *value) {
        stats->energy = *value;
    }
}

/*
 * 一轮 /v1/sync：发 outbox 里的前 COUPLE_EVENTS_PER_SYNC_MAX 条，解析回包。
 * 返回 true 表示这一轮成功（会被剪枝、对时、回调）；false 时 outbox 原样保留，
 * 下一轮复用同一批 seq 重发。
 * aborted（可传 NULL）为 true 表示这轮的长轮询是被 net_poke_poll() 自己掐断的，
 * 不是网络故障：调用方不要退避，直接重发。
 */
static bool net_sync_round(const couple_store_blob_t *snap, uint8_t *status_out, bool *aborted)
{
    couple_event_list_t list;
    uint8_t n = 0u;
    uint8_t i;
    uint32_t last_seq = 0u;
    const uint8_t holding_flag = s_holding ? (uint8_t)COUPLE_SYNC_FLAG_HOLDING : 0u;

    if (aborted != NULL) {
        *aborted = false;
    }

    memset(&list, 0, sizeof(list));

    portENTER_CRITICAL(&s_outbox_lock);
    for (i = 0u; i < s_outbox_count; i++) {
        if (n >= (uint8_t)COUPLE_EVENTS_PER_SYNC_MAX) {
            break;
        }
        list.items[n] = s_outbox[(uint8_t)((s_outbox_head + i) % (uint8_t)COUPLE_OUTBOX_CAPACITY)].ev;
        last_seq = s_outbox[(uint8_t)((s_outbox_head + i) % (uint8_t)COUPLE_OUTBOX_CAPACITY)].seq;
        n++;
    }
    portEXIT_CRITICAL(&s_outbox_lock);
    list.count = n;
    list.flags = holding_flag;

    /*
     * seq 规则：有事件就发这一批里最大的 seq（bump 出来的一定比对端记的大，
     * 因此 relay 会当成新批次 ingest）；没有事件时只有「抱抱状态变过」或
     * 「配对后第一次同步」才 bump，其它情况复用当前水位（relay 会当重传来
     * 跳过 ingest，但照样把对端事件和 server_time 带回来）。
     */
    uint32_t hdr_seq;
    if (n > 0u) {
        hdr_seq = last_seq;
    } else if (s_holding != s_holding_sent || snap->sync_seq == 0u) {
        hdr_seq = couple_store_bump_seq();
    } else {
        hdr_seq = snap->sync_seq;
    }

    const size_t plen = couple_proto_encode_sync_req(s_frame + COUPLE_PROTO_HDR_LEN,
                                                     sizeof(s_frame) - COUPLE_PROTO_HDR_LEN, &list,
                                                     holding_flag);
    if (plen == 0u) {
        ESP_LOGE(TAG, "同步请求编码失败");
        *status_out = COUPLE_NET_STATUS_LOCAL;
        return false;
    }
    const size_t flen = net_build_frame(COUPLE_FRAME_SYNC_REQ, hdr_seq, snap->sync_ack, plen,
                                        snap->pair_key);
    if (flen == 0u) {
        ESP_LOGE(TAG, "同步帧组不出来（seq=%u）", (unsigned)hdr_seq);
        *status_out = COUPLE_NET_STATUS_LOCAL;
        return false;
    }

    const int got = net_http_post(snap, "/v1/sync", s_frame, flen, snap->pair_id, NULL);
    if (got <= 0) {
        *status_out = COUPLE_NET_STATUS_NETWORK;
        if (s_http_poked) {
            /*
             * 这一轮是我们自己掐断的（新事件到了）。不当作失败：
             * 不打「同步失败」的日志（按一下刷一条会把真正的失败淹掉），
             * 由调用方直接重发。
             */
            s_http_poked = false;
            if (aborted != NULL) {
                *aborted = true;
            }
        }
        return false;
    }

    couple_frame_hdr_t hdr;
    if (!couple_proto_verify_frame(s_rsp, (size_t)got, snap->pair_key, &hdr) ||
        hdr.type != (uint8_t)COUPLE_FRAME_SYNC_RSP) {
        net_log_bad_frame(s_rsp, (size_t)got, "同步");
        *status_out = COUPLE_NET_STATUS_NETWORK;
        return false;
    }

    couple_sync_rsp_t rsp;
    if (!couple_proto_decode_sync_rsp(s_rsp + COUPLE_PROTO_HDR_LEN, (size_t)hdr.payload_len, &rsp)) {
        ESP_LOGW(TAG, "同步响应解码失败 (%u 字节)", (unsigned)hdr.payload_len);
        *status_out = COUPLE_NET_STATUS_NETWORK;
        return false;
    }

    *status_out = rsp.status;
    if (rsp.status != (uint8_t)COUPLE_STATUS_OK) {
        ESP_LOGW(TAG, "同步被 relay 拒绝: status=%u", (unsigned)rsp.status);
        return false;
    }

    /*
     * relay 回的 ack 就是它记住的 last_accepted，而只有「被 ingest 的批次」才会把
     * last_accepted 抬到我们的 seq；所以 ack >= hdr_seq 就等于整批（含更早的）
     * 都收下了，可以剪枝。
     */
    if (hdr.ack >= hdr_seq) {
        net_outbox_prune(hdr.ack);
        couple_store_apply_ack(hdr.ack);
    }

    s_holding_sent = s_holding;
    net_set_clock(rsp.server_time);

    /*
     * 用 relay 的权威值覆盖本机 Love Energy。只走 stats_update（回调里值不同才
     * 赋值），不调 stats_write()：这里在 worker 上，写盘自己会标脏并让下一次
     * 快照刷下去。
     */
    couple_store_stats_update(net_apply_relay_energy, &rsp.love_energy);

    if (s_hooks.on_sync != NULL) {
        couple_net_sync_t out;
        out.events = &rsp.events;
        out.partner_online = (rsp.partner_online != 0u);
        out.partner_holding = (rsp.partner_holding != 0u);
        out.pair_state = rsp.pair_state; /* PEER_LEFT 只透传给上层，本模块不自动解绑 */
        s_hooks.on_sync(&out, s_hooks.user);
    }
    return true;
}

/* ---------- worker ---------- */

static void net_task_exit(void)
{
    couple_wifi_set_cb(NULL, NULL);
    if (s_sta_cfg_valid) {
        couple_wifi_sta_stop();
        s_sta_cfg_valid = false;
    }
    s_sta_ssid[0] = '\0';

    if (s_http != NULL) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
        s_http_base[0] = '\0';
    }

    net_outbox_drop_all("网络 worker 退出");
    net_pair_session_clear();
    s_pair_req = NET_PAIR_REQ_NONE;
    s_unpair_req = false;
    s_have_ip = false;
    s_sync_failed = false;
    s_holding_sent = false;
    s_state = (uint8_t)COUPLE_NET_STATE_IDLE;
    s_state_reported = 0xFFu;
    s_started = false;

    ESP_LOGI(TAG, "网络 worker 退出");

    /* give 之后不能再碰队列：stop() 拿到信号量后可能马上就 start() 了。 */
    xSemaphoreGive(s_exit_sem);
}

static void net_task(void *arg)
{
    uint32_t retry_ms = COUPLE_RETRY_BASE_MS;

    (void)arg;
    ESP_LOGI(TAG, "网络 worker 启动");
    couple_wifi_set_cb(net_wifi_event_cb, NULL);

    for (;;) {
        couple_store_blob_t snap;

        if (!net_pump_commands()) {
            break;
        }

        couple_store_snapshot(&snap);
        net_apply_wifi(&snap);
        net_report_state();

        /*
         * 解绑排在 IP 检查之前：net_do_unpair() 自己处理「没网就只清本地」，
         * 断网时用户按了「解绑」不该等到 Wi-Fi 回来才生效。
         */
        if (s_unpair_req) {
            s_unpair_req = false;
            if (!net_do_unpair(&snap)) {
                break;
            }
            retry_ms = COUPLE_RETRY_BASE_MS;
            net_report_state();
            if (!net_wait_cmd(NET_IDLE_POLL_MS)) {
                break;
            }
            continue;
        }

        if (!s_have_ip) {
            if (!net_wait_cmd(NET_IDLE_POLL_MS)) {
                break;
            }
            continue;
        }

        if (!couple_store_is_paired()) {
            uint32_t wait_ms = NET_IDLE_POLL_MS;

            if (s_pair_active) {
                const esp_err_t err = net_await_step(&snap);
                if (err == ESP_OK) {
                    retry_ms = COUPLE_RETRY_BASE_MS;
                    wait_ms = NET_AWAIT_POLL_MS;
                } else {
                    wait_ms = net_backoff(&retry_ms);
                }
            } else if (s_pair_req == NET_PAIR_REQ_BEGIN) {
                s_pair_req = NET_PAIR_REQ_NONE;
                net_do_pair_begin(&snap);
                continue; /* 下一轮如果 s_pair_active 就会进 await */
            } else if (s_pair_req == NET_PAIR_REQ_CLAIM) {
                s_pair_req = NET_PAIR_REQ_NONE;
                net_do_pair_claim(&snap);
                continue;
            } else {
                retry_ms = COUPLE_RETRY_BASE_MS;
            }

            net_report_state();
            if (!net_wait_cmd(wait_ms)) {
                break;
            }
            continue;
        }

        {
            uint8_t status = COUPLE_NET_STATUS_NETWORK;
            bool aborted = false;
            const bool ok = net_sync_round(&snap, &status, &aborted);
            uint32_t wait_ms;

            if (aborted) {
                /*
                 * 这一轮的长轮询是我们自己掐断的（新事件/抱抱状态到了），不是网络故障：
                 * 不退避、不报失败状态——否则按一下就闪一次 OFFLINE——立刻重发一轮。
                 * retry_ms 也不重置：poke 不代表网络已经恢复，让它按自己的节奏增长，
                 * 下一次真成功自然会把它归位。
                 */
                s_sync_failed = false;
                wait_ms = 0u;
            } else if (ok) {
                s_sync_failed = false;
                retry_ms = COUPLE_RETRY_BASE_MS;
                /* outbox 里还有剩下的（超过一轮 12 条）就立刻再发一批。 */
                wait_ms = (couple_net_outbox_count() > 0u) ? 0u : (uint32_t)COUPLE_RETRY_BASE_MS;
            } else {
                s_sync_failed = true;
                wait_ms = net_backoff(&retry_ms);
                ESP_LOGW(TAG, "同步失败 (status=%u)，%u ms 后重试", (unsigned)status, (unsigned)wait_ms);
            }
            net_report_state();
            if (!net_wait_cmd(wait_ms)) {
                break;
            }
        }
    }

    net_task_exit();
    vTaskDelete(NULL);
}

/* ---------- 公开接口 ---------- */

esp_err_t couple_net_start(const couple_net_hooks_t *hooks)
{
    if (hooks == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_cmd_queue == NULL) {
        s_cmd_queue = xQueueCreate((UBaseType_t)NET_CMD_QUEUE_LEN, sizeof(net_cmd_t));
        if (s_cmd_queue == NULL) {
            ESP_LOGE(TAG, "命令队列创建失败");
            return ESP_ERR_NO_MEM;
        }
    }
    if (s_exit_sem == NULL) {
        s_exit_sem = xSemaphoreCreateBinary();
        if (s_exit_sem == NULL) {
            ESP_LOGE(TAG, "退出信号量创建失败");
            return ESP_ERR_NO_MEM;
        }
    }

    /* 队列与信号量跨实例复用，这里清掉上一次留下的残留。 */
    (void)xQueueReset(s_cmd_queue);
    (void)xSemaphoreTake(s_exit_sem, 0);

    s_hooks = *hooks;
    s_have_ip = false;
    s_sync_failed = false;
    s_holding = false;
    s_holding_sent = false;
    s_unpair_req = false;
    s_pair_req = NET_PAIR_REQ_NONE;
    s_pair_claim_code[0] = '\0';
    s_sta_cfg_valid = false;
    s_sta_ssid[0] = '\0';
    s_sta_retry_at_us = 0;
    s_state = (uint8_t)COUPLE_NET_STATE_IDLE;
    s_state_reported = 0xFFu;
    net_pair_session_clear();
    net_outbox_clear();
    s_started = true;

    if (xTaskCreate(net_task, "couple_net", NET_TASK_STACK, NULL, NET_TASK_PRIO, &s_task) != pdPASS) {
        s_task = NULL;
        s_started = false;
        ESP_LOGE(TAG, "网络任务创建失败");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t couple_net_stop(void)
{
    if (!s_started) {
        return ESP_OK;
    }

    net_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = (uint8_t)NET_CMD_STOP;
    (void)xQueueSend(s_cmd_queue, &cmd, 0);

    if (xSemaphoreTake(s_exit_sem, pdMS_TO_TICKS(NET_STOP_WAIT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "网络 worker 未在 %u ms 内退出（在途请求还没收尾？）", (unsigned)NET_STOP_WAIT_MS);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

couple_net_state_t couple_net_state(void)
{
    return (couple_net_state_t)s_state;
}

bool couple_net_is_online(void)
{
    return couple_net_state() == COUPLE_NET_STATE_ONLINE;
}

void couple_net_set_holding(bool holding)
{
    /*
     * 不能只置标志等下一轮：长轮询最长要 8 秒（本地超时 20 秒），抱抱状态
     * 要立刻送出去。掐断在飞的那一轮并叫醒 worker 重发。
     */
    s_holding = holding;
    net_poke_poll();
    (void)net_post_cmd(NET_CMD_KICK, NULL);
}

esp_err_t couple_net_enqueue_event(const couple_event_t *ev)
{
    net_outbox_item_t item;
    uint32_t dropped_seq = 0u;
    bool dropped = false;

    if (ev == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || !couple_store_is_paired()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ev->type != (uint8_t)COUPLE_EVENT_MISS && ev->type != (uint8_t)COUPLE_EVENT_HUG_START &&
        ev->type != (uint8_t)COUPLE_EVENT_HUG_END) {
        /* 服务端事件（HUG_SYNC 等）发出去会被 relay 整包拒收。 */
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * 归一化 value：relay 的 parse_sync_request 对每种事件的 value 长度是严格校验的
     * （MISS/HUG_START 必须 0 字节、HUG_END 必须 2 字节），多一个字节整包就被拒。
     */
    memset(&item, 0, sizeof(item));
    item.ev.type = ev->type;
    if (ev->type == (uint8_t)COUPLE_EVENT_HUG_END) {
        if (ev->len < 2u) {
            return ESP_ERR_INVALID_ARG;
        }
        item.ev.len = 2u;
        item.ev.value[0] = ev->value[0];
        item.ev.value[1] = ev->value[1];
    }
    /* MISS / HUG_START 一律 0 字节 value。 */
    item.seq = couple_store_bump_seq();

    portENTER_CRITICAL(&s_outbox_lock);
    if (s_outbox_count >= (uint8_t)COUPLE_OUTBOX_CAPACITY) {
        dropped_seq = s_outbox[s_outbox_head].seq;
        s_outbox_head = (uint8_t)((s_outbox_head + 1u) % (uint8_t)COUPLE_OUTBOX_CAPACITY);
        s_outbox_count--;
        dropped = true;
    }
    s_outbox[(uint8_t)((s_outbox_head + s_outbox_count) % (uint8_t)COUPLE_OUTBOX_CAPACITY)] = item;
    s_outbox_count++;
    portEXIT_CRITICAL(&s_outbox_lock);

    /* 临界区里不许打日志、不许碰 store。 */
    if (dropped) {
        ESP_LOGW(TAG, "outbox 已满，丢弃最旧的一条 (seq=%u)", (unsigned)dropped_seq);
    }

    /*
     * 事件已经入队，别让它等这一轮长轮询走完：掐断在飞的请求并唤醒 worker，
     * 毫秒级就发出去。队列满时 KICK 投不进去也无所谓——worker 马上会自己转回来。
     */
    net_poke_poll();
    (void)net_post_cmd(NET_CMD_KICK, NULL);
    return ESP_OK;
}

uint8_t couple_net_outbox_count(void)
{
    uint8_t n;

    portENTER_CRITICAL(&s_outbox_lock);
    n = s_outbox_count;
    portEXIT_CRITICAL(&s_outbox_lock);
    return n;
}

uint32_t couple_net_clock_now_utc(void)
{
    uint32_t base;
    int64_t base_us;
    bool synced;

    portENTER_CRITICAL(&s_clock_lock);
    base = s_clock_utc;
    base_us = s_clock_base_us;
    synced = s_clock_synced;
    portEXIT_CRITICAL(&s_clock_lock);

    if (!synced) {
        return 0u; /* 没对过时：调用方必须自己处理这个情况 */
    }
    {
        const int64_t elapsed_us = esp_timer_get_time() - base_us;
        return base + (uint32_t)((elapsed_us > 0) ? (uint64_t)elapsed_us / 1000000u : 0u);
    }
}

esp_err_t couple_net_pair_begin(void)
{
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pair_active || couple_store_is_paired()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_have_ip) {
        return ESP_ERR_INVALID_STATE;
    }
    return net_post_cmd(NET_CMD_PAIR_BEGIN, NULL);
}

esp_err_t couple_net_pair_claim(const char code[COUPLE_PAIR_CODE_LEN])
{
    char upper[COUPLE_PAIR_CODE_LEN + 1u];
    size_t i;

    if (code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pair_active || couple_store_is_paired()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_have_ip) {
        return ESP_ERR_INVALID_STATE;
    }

    for (i = 0u; i < (size_t)COUPLE_PAIR_CODE_LEN; i++) {
        char c = code[i];

        if (c == '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        if (c >= 'a' && c <= 'z') {
            c = (char)(c - 32);
        }
        upper[i] = c;
    }
    upper[COUPLE_PAIR_CODE_LEN] = '\0';

    return net_post_cmd(NET_CMD_PAIR_CLAIM, upper);
}

void couple_net_pair_cancel(void)
{
    /* 本地会话由 worker 收到命令后清掉；这里只是投一条命令，不报回调。 */
    (void)net_post_cmd(NET_CMD_PAIR_CANCEL, NULL);
}

esp_err_t couple_net_unpair(void)
{
    return net_post_cmd(NET_CMD_UNPAIR, NULL);
}

esp_err_t couple_net_settings_changed(void)
{
    return net_post_cmd(NET_CMD_WAKE, NULL);
}