#pragma once

/*
 * Heartlink 情侣心动终端 —— Wi-Fi / 网络栈的幂等准备步骤。
 *
 * 只做「初始化一次」这件事，不参与任何业务逻辑：
 *   - NVS 必须在 Wi-Fi 与任何持久化之前就绪
 *   - esp_netif 与默认事件循环按应用生命周期常驻，不需要反复创建
 *
 * 两个函数都可以被任意任务重复调用，内部用静态标志去重。
 */

#include <stdbool.h>

#include "esp_err.h"

/*
 * 幂等的 NVS 初始化。
 *
 * 只有在分区真的不可用时才擦除重建：
 *   ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND
 * 这两种情况下的擦除是唯一可行的恢复路径，且必须显式记录日志——
 * 因为擦除会连带清掉我们的身份与配对数据。
 * 其他错误一律原样上报，绝不擦除用户数据（与 demo_radio 的策略一致）。
 */
esp_err_t couple_nvs_prepare(void);

/* 幂等的 esp_netif_init() + esp_event_loop_create_default()。 */
esp_err_t couple_network_prepare(void);

/* 上一次 NVS 初始化是否走了擦除恢复路径（用于在屏幕上提示「数据已重置」）。 */
bool couple_nvs_was_wiped(void);