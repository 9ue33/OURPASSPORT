// main/main.c —— Heartlink 情侣心动终端:启动编排 + 按键分发。
//
// 本文件只负责三件事:
//   1. 按官方顺序把外设点起来(I2C -> 显示/LVGL -> 背光 -> 按键 -> 音频 -> 电池);
//   2. 把按键事件原样交给 couple_app(成分层见 couple_app.h);
//   3. 保留深睡按键唤醒的"按键动作守卫"接线。
//
// 所有产品逻辑(页面、声音、配对、网络、统计)都在 couple_app.c 里。官方 Demo 的
// 菜单、ui_pixel 外壳、demo_* 页面在本固件中不再编入,也不在本文件里出现。
#include "bsp_i2c.h"
#include "bsp_display.h"
#include "bsp_button.h"
#include "bsp_battery.h"
#include "bsp_pins.h"      // 错误日志里要打印 BSP_LCD_* 引脚号
#include "couple_app.h"
#include "couple_audio.h"
#include "couple_ui.h"
#include "key_guard.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "main";

#define INPUT_QUEUE_DEPTH 8

typedef struct {
    bsp_btn_t btn;
    bsp_btn_ev_t event;
} input_event_t;

static QueueHandle_t s_input_queue;
static TaskHandle_t s_input_task;
static volatile bool s_input_ready;

static void input_task(void *arg) {
    (void)arg;
    input_event_t input;
    for (;;) {
        if (xQueueReceive(s_input_queue, &input, portMAX_DELAY) == pdTRUE) {
            couple_app_handle_key(input.btn, input.event);
        }
    }
}

static esp_err_t input_dispatch_init(void) {
    s_input_queue = xQueueCreate(INPUT_QUEUE_DEPTH, sizeof(input_event_t));
    if (!s_input_queue) return ESP_ERR_NO_MEM;
    if (xTaskCreate(input_task, "couple_input", 4096, NULL, 5, &s_input_task) != pdPASS) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static void input_dispatch_deinit(void) {
    s_input_ready = false;
    if (s_input_task) {
        vTaskDelete(s_input_task);
        s_input_task = NULL;
    }
    if (s_input_queue) {
        vQueueDelete(s_input_queue);
        s_input_queue = NULL;
    }
}

// Waking from deep sleep by key reboots the chip while that key is still held: the
// button component starts a few hundred milliseconds later, registers a press that
// is already down, and grades it a long press once it has been down for its
// threshold (500 ms here), so an unintended action fires almost immediately. Drop
// key actions until that press comes up; the deadline is only a safety net so a
// stuck key cannot leave the pad dead. The state machine lives in key_guard.h so it
// can be unit-tested without the rest of the firmware.
#define KEY_GUARD_MAX_US (10LL * 1000 * 1000)
static key_guard_t s_key_guard;

// button callbacks run on the shared esp_timer task; enqueue only and return immediately.
static void on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user) {
    (void)user;
    // Consume the guard before the readiness check: a release can arrive after the
    // post-init voltage sample but before input dispatch is ready, and dropping it
    // here would leave the guard armed to swallow the next real press.
    const bool was_armed = s_key_guard.armed;
    if (key_guard_consume(&s_key_guard, ev == BSP_BTN_RELEASE, esp_timer_get_time())) {
        // The release and the deadline both arrive on this task; log the disarm
        // transition once it happens.
        if (was_armed && !s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键已松手,按键恢复响应");
        }
        return;
    }
    if (!s_input_ready || !s_input_queue) return;
    const input_event_t input = { .btn = btn, .event = ev };
    (void)xQueueSend(s_input_queue, &input, 0);
}

void app_main(void) {
    ESP_LOGI(TAG, "Heartlink 情侣心动终端启动");
    const uint32_t wake_causes = esp_sleep_get_wakeup_causes();
    if (wake_causes & ~(1u << ESP_SLEEP_WAKEUP_UNDEFINED)) {
        ESP_LOGI(TAG, "休眠唤醒原因位图: 0x%02x", (unsigned)wake_causes);
    }
    // 按键唤醒时,那颗键在整个启动期间还按着。先挡住它的动作(见 on_key)。
    if (wake_causes & (1u << ESP_SLEEP_WAKEUP_GPIO)) {
        key_guard_arm(&s_key_guard, esp_timer_get_time(), KEY_GUARD_MAX_US);
        ESP_LOGI(TAG, "按键唤醒:先屏蔽按键动作,直到那颗键松手");
    }

    bsp_i2c_init();
    bsp_i2c_scan();

    // 屏幕是唯一的交互载体,失败就没有界面可言 —— 打清楚日志后退出,
    // 不做"串口菜单"降级(设备没有串口终端的使用场景)。
    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,设备无法继续。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    // 按键链路:先建队列与任务,再注册回调 —— 回调一旦注册就可能立刻投递事件。
    esp_err_t input_err = input_dispatch_init();
    esp_err_t button_err = input_err == ESP_OK
                         ? bsp_button_init(on_key, NULL)
                         : ESP_ERR_INVALID_STATE;
    if (input_err != ESP_OK) {
        ESP_LOGE(TAG, "按键事件任务创建失败: %s", esp_err_to_name(input_err));
    } else if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        input_dispatch_deinit();
    } else if (s_key_guard.armed) {
        // 若唤醒键在按键驱动启动前就已松手,组件不会产生 release 事件,守卫会一直
        // 拦到兜底。初始化完成后按真实电压同步一次:已松手立即撤防。
        const bool released = bsp_button_read_mv() >= BSP_BTN_MV_RELEASED_MIN;
        key_guard_sync_released(&s_key_guard, released);
        if (!s_key_guard.armed) {
            ESP_LOGI(TAG, "唤醒键在按键驱动启动前已松手,提前撤防");
        }
    }

    // couple_audio 是 bsp_audio 的唯一所有者,这里不再直接调 bsp_audio_*。
    // 音频失败只是没声音,界面照常可用,所以不阻塞启动。
    const esp_err_t audio_err = couple_audio_init();
    if (audio_err != ESP_OK) {
        ESP_LOGW(TAG, "音频初始化失败,静音运行: %s", esp_err_to_name(audio_err));
    }

    const esp_err_t battery_err = bsp_battery_init();
    if (battery_err != ESP_OK) {
        ESP_LOGW(TAG, "电量计初始化失败: %s", esp_err_to_name(battery_err));
    }

    // 界面先建,编排层后起:couple_app_init() 会立刻推一次状态并切到首页。
    esp_err_t err = couple_ui_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "界面初始化失败: %s", esp_err_to_name(err));
        return;
    }
    err = couple_app_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "应用初始化失败: %s", esp_err_to_name(err));
        return;
    }

    // 到这里界面与状态机都已就绪,放行按键。此前的按键事件已经被丢弃。
    s_input_ready = true;

    ESP_LOGI(TAG, "就绪:Button=%s Audio=%s Battery=%s",
             button_err == ESP_OK ? "ok" : "fail",
             audio_err == ESP_OK ? "ok" : "fail",
             battery_err == ESP_OK ? "ok" : "fail");
}