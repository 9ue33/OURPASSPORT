/*
 * Heartlink 情侣心动终端 —— Wi-Fi 驱动实现。
 *
 * 事实依据（逐行读过 esp-idf/components/esp_wifi/src/wifi_default.c）：
 *   esp_netif_create_default_wifi_sta() / _ap() 只会安装 netif 动作处理器 ——
 *   wifi_default_action_sta_start() → wifi_start()（该文件 43-82 行）只做
 *   mac 注册与 esp_netif_action_start()，**不会**替我们调用 esp_wifi_connect()；
 *   wifi_default_action_sta_disconnected() 同样只做 esp_netif_action_disconnected()。
 *   而且 set_default_wifi_handlers()（217 行）带 wifi_default_handlers_set 幂等保护，
 *   所以 STA 与 AP 的默认处理器可以共存，两个 netif 都能在启动时一次建好。
 *   结论：「断线关联重连 + 指数退避」必须由本文件自己实现，不能依赖驱动。
 *
 * 模式切换统一走 wifi_apply()：stop → set_mode → set_config → start。
 * Wi-Fi 只 init 一次，之后不再 deinit，避免 ESP32-C3 上反复启停造成的内存碎片。
 */

#include "couple_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "couple_config.h"
#include "couple_radio.h"

static const char *TAG = "couple.wifi";

#define COUPLE_WIFI_AP_MAX_STA 4
#define COUPLE_WIFI_AP_CHANNEL 1

static bool s_ready;                        /* 驱动已 init 且处理器已注册 */
static bool s_wifi_started;                 /* esp_wifi_start() 成功过且未 stop */
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static esp_event_handler_instance_t s_wifi_ev_inst;
static esp_event_handler_instance_t s_ip_ev_inst;
static couple_wifi_cb_t s_cb;
static void *s_cb_user;
static char s_ap_ssid[33];
static volatile bool s_ap_running;
static volatile bool s_sta_wanted;          /* 用户是否希望保持 STA 连接 */
static volatile bool s_sta_has_ip;
static volatile uint8_t s_ap_clients;
static esp_timer_handle_t s_retry_timer;
static uint32_t s_retry_delay_ms;

static void notify(couple_wifi_event_t ev, uint8_t reason)
{
    couple_wifi_cb_t cb = s_cb;

    if (cb != NULL) {
        cb(ev, reason, s_cb_user);
    }
}

/* ---------- 关联重连（我们自己实现，驱动不管这件事） ---------- */

static void retry_timer_cb(void *arg)
{
    (void)arg;

    if (!s_ready || !s_sta_wanted || !s_wifi_started) {
        return;
    }

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "重连触发失败：%s", esp_err_to_name(err));
    }
}

static void schedule_retry(void)
{
    if (!s_ready || !s_sta_wanted || !s_wifi_started || s_retry_timer == NULL) {
        return;
    }

    uint32_t delay = s_retry_delay_ms;
    /* 加四分之一以内的随机抖动，避免两台设备同时抢信道 */
    delay += esp_random() % (delay / 4U + 1U);

    if (s_retry_delay_ms < COUPLE_WIFI_RETRY_MAX_MS) {
        uint32_t next = s_retry_delay_ms * 2U;
        s_retry_delay_ms = (next > COUPLE_WIFI_RETRY_MAX_MS) ? COUPLE_WIFI_RETRY_MAX_MS : next;
    }

    (void)esp_timer_stop(s_retry_timer); /* 未在运行时会返回错误，忽略 */
    esp_err_t err = esp_timer_start_once(s_retry_timer, (uint64_t)delay * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "重连定时器启动失败：%s", esp_err_to_name(err));
    }
}

/* ---------- 事件处理 ---------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            /* 驱动启动 STA 后不会自己发起关联，这里补上 */
            if (s_sta_wanted) {
                esp_err_t err = esp_wifi_connect();
                if (err != ESP_OK) {
                    ESP_LOGD(TAG, "首次关联触发失败：%s", esp_err_to_name(err));
                }
            }
            break;

        case WIFI_EVENT_STA_CONNECTED:
            notify(COUPLE_WIFI_EV_STA_CONNECTED, 0);
            break;

        case WIFI_EVENT_STA_DISCONNECTED: {
            const wifi_event_sta_disconnected_t *ev = (const wifi_event_sta_disconnected_t *)data;
            uint8_t reason = (ev != NULL) ? ev->reason : 0;

            s_sta_has_ip = false;
            notify(COUPLE_WIFI_EV_STA_DISCONNECTED, reason);

            /* 退避已经涨到上限说明家里 Wi-Fi 或密码大概率有问题，值得提醒一次 */
            if (s_retry_delay_ms >= COUPLE_WIFI_RETRY_MAX_MS) {
                ESP_LOGW(TAG, "Wi-Fi 反复连不上（reason=%u），仍在后台重试", (unsigned)reason);
            } else {
                ESP_LOGD(TAG, "Wi-Fi 断开（reason=%u），稍后重连", (unsigned)reason);
            }

            schedule_retry();
            break;
        }

        case WIFI_EVENT_AP_STACONNECTED:
            if (s_ap_clients < UINT8_MAX) {
                s_ap_clients++;
            }
            ESP_LOGI(TAG, "配网热点有新客户端接入");
            break;

        case WIFI_EVENT_AP_STADISCONNECTED:
            if (s_ap_clients > 0) {
                s_ap_clients--;
            }
            break;

        default:
            break;
        }
        return;
    }

    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_sta_has_ip = true;
        s_retry_delay_ms = COUPLE_WIFI_RETRY_BASE_MS; /* 连上了就把退避清零 */
        notify(COUPLE_WIFI_EV_STA_GOT_IP, 0);
    }
}

/* ---------- 模式切换 ---------- */

/*
 * 唯一允许动驱动状态的地方。
 * mode == WIFI_MODE_NULL 表示关掉射频，不再 start。
 * cfg 无 const：esp_wifi_set_config() 的原型就要求可写指针（IDF 5.5.3
 * esp_wifi.h:1005），传 const 会触发 -Wdiscarded-qualifiers。
 */
static esp_err_t wifi_apply(wifi_mode_t mode, wifi_interface_t iface, wifi_config_t *cfg)
{
    esp_err_t err;

    if (s_wifi_started) {
        err = esp_wifi_stop();
        if (err != ESP_OK) {
            return err;
        }
        s_wifi_started = false;
    }

    s_sta_has_ip = false;
    if (mode != WIFI_MODE_AP) {
        s_ap_running = false;
        s_ap_clients = 0;
    }

    err = esp_wifi_set_mode(mode);
    if (err != ESP_OK) {
        return err;
    }

    if (mode == WIFI_MODE_NULL) {
        return ESP_OK;
    }

    if (cfg != NULL) {
        err = esp_wifi_set_config(iface, cfg);
        if (err != ESP_OK) {
            return err;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }
    s_wifi_started = true;
    return ESP_OK;
}

/* ---------- 对外接口 ---------- */

esp_err_t couple_wifi_begin(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = couple_network_prepare();
    if (err != ESP_OK) {
        return err;
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    /* 凭据由本工程自己存 NVS，不让驱动往 NVS 里写 */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        (void)esp_wifi_deinit();
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_ap_netif == NULL || s_sta_netif == NULL) {
        if (s_ap_netif != NULL) {
            esp_netif_destroy(s_ap_netif);
            s_ap_netif = NULL;
        }
        if (s_sta_netif != NULL) {
            esp_netif_destroy(s_sta_netif);
            s_sta_netif = NULL;
        }
        (void)esp_wifi_deinit();
        return ESP_FAIL;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL,
                                             &s_wifi_ev_inst);
    if (err != ESP_OK) {
        (void)couple_wifi_end();
        return err;
    }

    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL,
                                             &s_ip_ev_inst);
    if (err != ESP_OK) {
        (void)couple_wifi_end();
        return err;
    }

    esp_timer_create_args_t timer_args = {
        .callback = retry_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "couple_wifi",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&timer_args, &s_retry_timer);
    if (err != ESP_OK) {
        (void)couple_wifi_end();
        return err;
    }

    s_retry_delay_ms = COUPLE_WIFI_RETRY_BASE_MS;
    s_ap_ssid[0] = '\0';
    s_ready = true;
    ESP_LOGI(TAG, "Wi-Fi 驱动就绪");
    return ESP_OK;
}

void couple_wifi_end(void)
{
    if (!s_ready) {
        return;
    }

    s_ready = false;
    s_sta_wanted = false;

    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
        (void)esp_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }

    if (s_wifi_ev_inst != NULL) {
        (void)esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, s_wifi_ev_inst);
        s_wifi_ev_inst = NULL;
    }
    if (s_ip_ev_inst != NULL) {
        (void)esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, s_ip_ev_inst);
        s_ip_ev_inst = NULL;
    }

    if (s_wifi_started) {
        (void)esp_wifi_stop();
        s_wifi_started = false;
    }
    (void)esp_wifi_deinit();

    if (s_ap_netif != NULL) {
        esp_netif_destroy(s_ap_netif);
        s_ap_netif = NULL;
    }
    if (s_sta_netif != NULL) {
        esp_netif_destroy(s_sta_netif);
        s_sta_netif = NULL;
    }

    s_sta_has_ip = false;
    s_ap_running = false;
    s_ap_clients = 0;
    s_ap_ssid[0] = '\0';
}

void couple_wifi_set_cb(couple_wifi_cb_t cb, void *user)
{
    s_cb_user = user;
    s_cb = cb;
}

/* ---------- AP 配网模式 ---------- */

/* ssid 为空时用 MAC 末两字节拼出 "Heartlink-XXXX"。 */
static esp_err_t compose_ap_ssid(const char *ssid, char out[33])
{
    if (ssid != NULL && ssid[0] != '\0') {
        size_t len = strlen(ssid);
        if (len > 32) {
            return ESP_ERR_INVALID_ARG;
        }
        memmove(out, ssid, len + 1);
        return ESP_OK;
    }

    uint8_t mac[6] = {0};
    esp_err_t err = esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    if (err != ESP_OK) {
        return err;
    }
    snprintf(out, 33, "%s%02X%02X", "Heartlink-", mac[4], mac[5]);
    return ESP_OK;
}

esp_err_t couple_wifi_ap_start(const char *ssid, const char *pass)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    const char *password = (pass != NULL) ? pass : "";
    size_t pass_len = strlen(password);
    if (pass_len != 0 && (pass_len < 8 || pass_len > 63)) {
        return ESP_ERR_INVALID_ARG;
    }

    char name[33] = {0};
    esp_err_t err = compose_ap_ssid(ssid, name);
    if (err != ESP_OK) {
        return err;
    }
    size_t ssid_len = strlen(name);
    if (ssid_len == 0 || ssid_len > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    memcpy(cfg.ap.ssid, name, ssid_len);
    cfg.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(cfg.ap.password, password, pass_len);
    cfg.ap.channel = COUPLE_WIFI_AP_CHANNEL;
    cfg.ap.authmode = (pass_len != 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    /* 别设成 1：手机掉线后槽位回收慢会导致重连被拒 */
    cfg.ap.max_connection = COUPLE_WIFI_AP_MAX_STA;
    cfg.ap.pmf_cfg.required = false;

    /* AP 与 STA 不同时跑，先掐掉 STA 侧的重连意图 */
    s_sta_wanted = false;
    s_ap_clients = 0;

    err = wifi_apply(WIFI_MODE_AP, WIFI_IF_AP, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    memcpy(s_ap_ssid, name, sizeof(s_ap_ssid));
    s_ap_running = true;
    ESP_LOGI(TAG, "配网热点已开启：%s", s_ap_ssid);
    return ESP_OK;
}

void couple_wifi_ap_stop(void)
{
    if (!s_ready) {
        return;
    }
    if (!s_ap_running && !s_wifi_started) {
        return;
    }

    esp_err_t err = wifi_apply(WIFI_MODE_NULL, WIFI_IF_AP, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "关闭配网热点失败：%s", esp_err_to_name(err));
    }
    s_ap_running = false;
    s_ap_clients = 0;
    s_ap_ssid[0] = '\0';
}

bool couple_wifi_ap_running(void)
{
    return s_ap_running;
}

const char *couple_wifi_ap_ssid(void)
{
    return s_ap_ssid;
}

uint8_t couple_wifi_ap_client_count(void)
{
    return s_ap_clients;
}

/* ---------- STA 日常模式 ---------- */

esp_err_t couple_wifi_sta_start(const char *ssid, const char *pass)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ssid == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t ssid_len = strlen(ssid);
    if (ssid_len == 0 || ssid_len > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *password = (pass != NULL) ? pass : "";
    size_t pass_len = strlen(password);
    if (pass_len > 63 || (pass_len != 0 && pass_len < 8)) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, ssid_len);
    memcpy(cfg.sta.password, password, pass_len);

    s_retry_delay_ms = COUPLE_WIFI_RETRY_BASE_MS;
    s_sta_wanted = true;

    /* STA_START 事件里会自己发起关联，这里只负责把配置下发 */
    esp_err_t err = wifi_apply(WIFI_MODE_STA, WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        s_sta_wanted = false;
        ESP_LOGE(TAG, "切换到 STA 模式失败：%s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "开始连接 Wi-Fi：%s", ssid);
    return ESP_OK;
}

void couple_wifi_sta_stop(void)
{
    if (!s_ready) {
        return;
    }

    s_sta_wanted = false;
    if (s_retry_timer != NULL) {
        (void)esp_timer_stop(s_retry_timer);
    }

    if (!s_wifi_started) {
        return;
    }

    esp_err_t err = wifi_apply(WIFI_MODE_NULL, WIFI_IF_STA, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "关闭 Wi-Fi 失败：%s", esp_err_to_name(err));
    }
}

bool couple_wifi_sta_has_ip(void)
{
    return s_sta_has_ip;
}

int couple_wifi_sta_rssi(void)
{
    if (!s_sta_has_ip) {
        return 0;
    }

    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}