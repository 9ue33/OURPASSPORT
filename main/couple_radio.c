#include "couple_radio.h"

#include <stdbool.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"

static const char *TAG = "couple_radio";

static bool s_nvs_ready;
static bool s_nvs_wiped;
static bool s_netif_ready;
static bool s_event_loop_ready;

bool couple_nvs_was_wiped(void)
{
    return s_nvs_wiped;
}

esp_err_t couple_nvs_prepare(void)
{
    if (s_nvs_ready) {
        return ESP_OK;
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 分区不可用(%s),擦除后重建", esp_err_to_name(err));
        esp_err_t erase_err = nvs_flash_erase();
        if (erase_err != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败: %s", esp_err_to_name(erase_err));
            return erase_err;
        }
        err = nvs_flash_init();
        if (err == ESP_OK) {
            s_nvs_wiped = true;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_nvs_ready = true;
    return ESP_OK;
}

esp_err_t couple_network_prepare(void)
{
    if (!s_netif_ready) {
        esp_err_t err = esp_netif_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_netif_init 失败: %s", esp_err_to_name(err));
            return err;
        }
        s_netif_ready = true;
    }

    if (!s_event_loop_ready) {
        esp_err_t err = esp_event_loop_create_default();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "默认事件循环创建失败: %s", esp_err_to_name(err));
            return err;
        }
        s_event_loop_ready = true;
    }

    return ESP_OK;
}