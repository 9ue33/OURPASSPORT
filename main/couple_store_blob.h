#pragma once

/*
 * Heartlink 情侣心动终端 —— 落 NVS 的那一份数据模型（纯 C，无 ESP-IDF 依赖）。
 *
 * 这里只放「数据长什么样」和「脏数据怎么修」的纯逻辑，真正的 NVS 读写在看
 * couple_store.h。这样拆开的好处是：结构校验、名称清洗、地址规范化这些最容易出
 * 错的地方可以在宿主机上直接编译测试，不需要芯片。
 *
 * 注意：本结构体会被原样写进 NVS，字段顺序就是磁盘布局。
 * 任何字段的增删改都必须同时抬高 COUPLE_STORE_SCHEMA，并在 couple_store.c 里
 * 写好迁移或重建策略 —— couple_store_blob.c 末尾的 _Static_assert 会替你把关。
 *
 * 设计红线：统计数据坏掉可以重来，配对关系不能因为统计数据坏掉而被清掉。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "couple_state.h"
#include "couple_types.h"

#define COUPLE_STORE_MAGIC 0x484C4944u /* "HLID" */
#define COUPLE_STORE_SCHEMA 1u

/* flags 位定义 */
#define COUPLE_STORE_FLAG_PAIRED 0x0001u   /* 已与对方绑定 */
#define COUPLE_STORE_FLAG_HAS_WIFI 0x0002u /* 存了可用的 Wi-Fi 凭据 */

/* 配对共享密钥就是 32 字节，复用协议里的长度常量。 */
#define COUPLE_STORE_PAIR_KEY_LEN COUPLE_PAIR_TOKEN_LEN

typedef struct {
    uint32_t magic;
    uint16_t schema;
    uint16_t flags;

    /* 本机身份：一次生成，永不变，解除配对也不重新生成。 */
    uint8_t device_id[COUPLE_DEVICE_ID_LEN];

    uint8_t pair_id[COUPLE_PAIR_ID_LEN];
    uint8_t pair_key[COUPLE_STORE_PAIR_KEY_LEN];
    uint8_t partner_id[COUPLE_DEVICE_ID_LEN];

    char partner_name[COUPLE_NAME_MAX + 1]; /* 对方名字，UTF-8 */
    char self_name[COUPLE_NAME_MAX + 1];    /* 本机名字，UTF-8 */

    char relay_url[COUPLE_RELAY_URL_MAX];
    char wifi_ssid[COUPLE_WIFI_SSID_MAX];
    char wifi_pass[COUPLE_WIFI_PASS_MAX];

    /* 一定要在配对建立后非零，用于防重放：对端只接受比上次更大的 seq。 */
    uint32_t sync_seq;
    uint32_t sync_ack;

    couple_stats_t stats;
} couple_store_blob_t;

/* ---------- 纯逻辑 ---------- */

/* 重置为出厂状态（全部清零 + 写上 magic/schema）。device_id 由上层用真随机填。 */
void couple_store_blob_init(couple_store_blob_t *blob);

/* magic、schema 与「已配对时三元组不能全零」这三条不变量。 */
bool couple_store_blob_valid(const couple_store_blob_t *blob);

bool couple_store_blob_is_paired(const couple_store_blob_t *blob);

/* 发一次同步，seq 单调自增（0 保留给「还没发过」）。 */
uint32_t couple_store_blob_next_seq(couple_store_blob_t *blob);

/* 记录对端确认到的位置。只增不减，乱序到达的响应不会把剪枝点拉回去。 */
void couple_store_blob_set_ack(couple_store_blob_t *blob, uint32_t ack);

/* 该 device_id 是否还是全零（尚未生成）。 */
bool couple_store_blob_id_is_empty(const uint8_t *id, size_t len);

/* ---------- 字符串清洗 ---------- */

/*
 * 安全拷贝：最多写入 cap-1 字节 + NUL。
 * src 为 NULL 时写空串。返回写入的字节数。
 */
size_t couple_store_copy(char *out, size_t cap, const char *src);

/*
 * 名字清洗：丢掉控制字符，补齐首尾空白，连续空白压成一个空格，
 * 超过容量时只在 UTF-8 字符边界截断（绝不写出半个汉字）。
 * 返回写入的字节数；out 永远以 NUL 结尾。
 */
size_t couple_store_sanitize_name(char *out, size_t cap, const char *src);

/*
 * relay 地址规范化：只接受 http:// 或 https:// 开头且带主机名，
 * 去掉尾部斜杠。成功返回 true；失败时 out 写空串。
 */
bool couple_store_sanitize_relay_url(char *out, size_t cap, const char *src);