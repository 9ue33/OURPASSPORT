#pragma once

/*
 * Heartlink 情侣心动终端 —— Wi-Fi 驱动的唯一持有者。
 *
 * 全应用只在启动时调用一次 couple_wifi_begin()，此后在两种角色之间切换：
 *   AP  配网模式：手机连上设备热点，打开配置页填家里 Wi-Fi 与 relay 地址
 *   STA 日常模式：连上家里 Wi-Fi，和 relay 长轮询
 *
 * 驱动实例只 init 一次，切换模式不再重复 init/deinit，避免 ESP32-C3 上
 * Wi-Fi 反复启停带来的内存碎片。
 *
 * 凭据一律由本工程自己存 NVS（WIFI_STORAGE_RAM），不使用驱动的 NVS 存储。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    COUPLE_WIFI_EV_STA_CONNECTED = 1,    /* 已和 AP 完成关联 */
    COUPLE_WIFI_EV_STA_GOT_IP = 2,       /* 拿到 IP，可以开始发请求 */
    COUPLE_WIFI_EV_STA_DISCONNECTED = 3, /* 断线，参数里带原因码 */
} couple_wifi_event_t;

/*
 * 事件回调。运行在默认事件循环任务里，
 * 只允许做入队这类瞬时动作，绝不能在里面做网络/音频/Flash 操作。
 * reason 仅 COUPLE_WIFI_EV_STA_DISCONNECTED 有意义，其余为 0。
 */
typedef void (*couple_wifi_cb_t)(couple_wifi_event_t ev, uint8_t reason, void *user);

/* 初始化驱动（幂等）。必须在 couple_network_prepare() 之后调用。 */
esp_err_t couple_wifi_begin(void);

/* 释放驱动。深睡前使用，正常运行时不要调用。 */
void couple_wifi_end(void);

/* 注册/清除事件回调。cb 传 NULL 表示清除。 */
void couple_wifi_set_cb(couple_wifi_cb_t cb, void *user);

/* ---------- AP 配网模式 ---------- */

/*
 * 起一个 SoftAP。ssid 为空时自动用 "Heartlink-XXXX"（XXXX = MAC 末两字节）。
 * pass 长度必须为 0（开放）或 8..63（WPA2）。重复调用会先停掉旧的 AP。
 */
esp_err_t couple_wifi_ap_start(const char *ssid, const char *pass);
void couple_wifi_ap_stop(void);
bool couple_wifi_ap_running(void);

/* 当前 AP 的 SSID（未运行返回空串）。 */
const char *couple_wifi_ap_ssid(void);

/* 当前连在 AP 上的客户端数量，用于配网页提示「手机已连接」。 */
uint8_t couple_wifi_ap_client_count(void);

/* ---------- STA 日常模式 ---------- */

/*
 * 切到 STA 并连接指定 AP。函数只负责把配置下发并触发连接，不等待结果；
 * 连接结果通过事件回调上报。重复调用会重新配置并重连。
 */
esp_err_t couple_wifi_sta_start(const char *ssid, const char *pass);
void couple_wifi_sta_stop(void);

/* 已关联且已拿到 IP。 */
bool couple_wifi_sta_has_ip(void);

/* 当前 RSSI（dBm）；未连接返回 0。 */
int couple_wifi_sta_rssi(void);