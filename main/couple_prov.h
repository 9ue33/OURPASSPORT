#pragma once

/*
 * Heartlink 情侣心动终端 —— SoftAP 配网（热点 + 手机配置页）。
 *
 * 职责只做一件事：把「家里的 Wi-Fi 名称 / 密码 + Heartlink 服务器地址 + 你的名字」
 * 从手机搬到设备上。完整流程：
 *   1. couple_prov_start()：起 SoftAP（Heartlink-XXXX）+ HTTP 配置页（80 端口）
 *   2. 手机连上这个热点，用浏览器打开 http://192.168.4.1/
 *   3. 填完点保存 → on_saved() 把四个字段交回应用（存哪里由应用决定）
 *   4. 应用处理完自己调 couple_prov_stop()，热点关闭，再切 STA 日常模式
 *
 * 边界与约束（都是刻意的，改动前先读这里）：
 *   - 本模块不直接碰 Wi-Fi 驱动，一切经 couple_wifi.h。
 *     AP 与 STA 不同时运行：应用必须先 couple_prov_stop()，
 *     再调 couple_wifi_sta_start()。
 *   - 本模块不持久化任何凭据，只回调。存 NVS 是应用的事。
 *   - on_saved() 跑在 httpd 任务里（栈 6144 字节）：
 *       * 传入的四个字符串指向模块静态缓冲，只在回调期间有效，
 *         要留存必须自己拷贝；name 可能是空串（表示保持当前名字）；
 *       * 不要在里面阻塞（NVS 落盘请入队交给工作线程）；
 *       * 绝对不要在里面调 couple_prov_stop() / couple_prov_start() ——
 *         httpd_stop() 会等当前任务退出，直接死锁。
 *   - on_status() 跑在 esp_timer 任务里，同样禁止阻塞。
 *   - 「手机接入 / 断开」由 1 Hz 轮询 couple_wifi_ap_client_count() 得到
 *     （couple_wifi 只上报 STA 事件，没有 AP 客户端事件），
 *     所以 on_status() 最多有约 1 秒延迟。
 *   - 日志绝不打印 Wi-Fi 密码；SSID、服务器地址与名字可以打。
 *   - 静态内存占用约 5.3 KB（ESP32-C3 无 PSRAM，页面上限按此定死）。
 */

#include <stdbool.h>

#include "esp_err.h"

/* 热点名前缀。实际 SSID 由 couple_wifi 按 MAC 末两字节补成 "Heartlink-XXXX"。 */
#define COUPLE_PROV_AP_SSID_PREFIX "Heartlink-"
/* 热点密码，WPA2 要求至少 8 个字符。 */
#define COUPLE_PROV_AP_PASS "heartlink"
/* 配置页端口。手机浏览器默认按 80 端口访问，别改。 */
#define COUPLE_PROV_HTTP_PORT 80

typedef struct {
    /*
     * 用户在配置页点了保存，且必填字段都通过了长度与格式校验。
     * 四个字符串都指向模块静态缓冲，仅在本次调用期间有效。
     * name 可能是空串：表示用户没填，保持当前名字不变（不是清空）。
     * 不会被调用的情形：字段缺失、URL 解码失败、校验不通过。
     */
    void (*on_saved)(const char *ssid, const char *pass, const char *relay_url, const char *name,
                     void *user);

    /* 手机上/下热点。start 成功后立刻按当前状态先报一次。 */
    void (*on_status)(bool client_connected, void *user);

    void *user;
} couple_prov_hooks_t;

/*
 * 起热点与配置页。hooks 按值拷贝，调用方不必保持结构体存活。
 *
 * 内部顺序：NVS 就绪 → 网络栈就绪 → Wi-Fi 驱动就绪 → 起 AP → 起 httpd。
 * current_relay_url 是当前生效的服务器地址，用来预填表单；
 * 传 NULL 或空串时预填 COUPLE_DEFAULT_RELAY_URL。过长的地址会被静默截断。
 * self_name 是当前生效的本机名字，用来预填「你的名字」输入框；
 * 传 NULL 或空串时预填空串（用户不填就保持原值）。过长会被静默截断到
 * COUPLE_NAME_MAX 字节。
 *
 * 幂等：已经激活时直接返回 ESP_OK，不重复起服务。
 * 任何一步失败都会把前面的步骤收回去（含关掉已开的热点），
 * 失败时不会留下半启动状态。
 */
esp_err_t couple_prov_start(const couple_prov_hooks_t *hooks, const char *current_relay_url,
                            const char *self_name);

/*
 * 停 httpd 与热点。没启动时调用安全，重复调用安全，
 * 返回后保证不再有任何回调（含 on_status），标志位复位以便再次 start。
 */
void couple_prov_stop(void);

/* 配网是否处于「服务可用」状态（start 成功之后、stop 之前）。 */
bool couple_prov_active(void);

/*
 * 当前热点 SSID；未激活时返回空串。指针在 stop 之前有效。
 * 屏幕上显示「请连 Heartlink-XXXX」时用它，不要自己拼。
 */
const char *couple_prov_ap_ssid(void);