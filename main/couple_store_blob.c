/*
 * Heartlink 情侣心动终端 —— NVS 数据模型的纯逻辑实现。
 *
 * 这里不碰 ESP-IDF，也不碰 NVS：只负责「结构是不是好的」「名字怎么写才安全」
 * 「地址怎么规范化」「seq 怎么单调递增」。这样这些最容易出错的地方能在宿主机上
 * 直接编译测试。
 */

#include "couple_store_blob.h"

#include <string.h>

/* ---------- 内部小工具 ---------- */

static bool bytes_all_zero(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return true;
    }
    for (size_t i = 0u; i < len; ++i) {
        if (data[i] != 0u) {
            return false;
        }
    }
    return true;
}

/* 配对三元组（pair_id / pair_key / partner_id）必须齐全，缺一不可。 */
static bool pair_trio_ok(const couple_store_blob_t *blob)
{
    if (bytes_all_zero(blob->pair_id, COUPLE_PAIR_ID_LEN)) {
        return false;
    }
    if (bytes_all_zero(blob->pair_key, COUPLE_STORE_PAIR_KEY_LEN)) {
        return false;
    }
    if (bytes_all_zero(blob->partner_id, COUPLE_DEVICE_ID_LEN)) {
        return false;
    }
    return true;
}

/*
 * 从 s 起算一个 UTF-8 字符占多少字节。
 *   返回值 1..4  可以整段安全拷贝
 *   返回值 0     尾部被截断，调用方必须停止（宁可少一个字，也不写出半个汉字）
 */
static size_t utf8_seq_len(const char *s, size_t remaining)
{
    if (remaining == 0u) {
        return 0u;
    }

    const unsigned char b = (unsigned char)s[0];
    size_t need;

    if (b < 0x80u) {
        return 1u;
    } else if (b >= 0xC2u && b <= 0xDFu) {
        need = 2u;
    } else if (b >= 0xE0u && b <= 0xEFu) {
        need = 3u;
    } else if (b >= 0xF0u && b <= 0xF4u) {
        need = 4u;
    } else {
        return 1u; /* 非法首字节：按单字节原样带过，不会切坏合法字符 */
    }

    if (need > remaining) {
        return 0u;
    }
    for (size_t k = 1u; k < need; ++k) {
        if (((unsigned char)s[k] & 0xC0u) != 0x80u) {
            return 1u; /* 续字节坏了：这里本来就不是一个合法字符 */
        }
    }
    return need;
}

/* 大小写不敏感的前缀比较（只处理 ASCII）。 */
static bool starts_with_ci(const char *s, const char *prefix, size_t prefix_len)
{
    for (size_t i = 0u; i < prefix_len; ++i) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (a != prefix[i]) {
            return false;
        }
    }
    return true;
}

/* ---------- 结构与序号 ---------- */

void couple_store_blob_init(couple_store_blob_t *blob)
{
    if (blob == NULL) {
        return;
    }
    memset(blob, 0, sizeof(*blob));
    blob->magic = COUPLE_STORE_MAGIC;
    blob->schema = (uint16_t)COUPLE_STORE_SCHEMA;
    blob->flags = 0u;
    couple_state_init(&blob->stats);
}

bool couple_store_blob_valid(const couple_store_blob_t *blob)
{
    if (blob == NULL) {
        return false;
    }
    if (blob->magic != COUPLE_STORE_MAGIC) {
        return false;
    }
    if (blob->schema != (uint16_t)COUPLE_STORE_SCHEMA) {
        return false;
    }
    /* 声称已配对，就必须拿得出完整的配对三元组。 */
    if ((blob->flags & COUPLE_STORE_FLAG_PAIRED) != 0u && !pair_trio_ok(blob)) {
        return false;
    }
    return true;
}

bool couple_store_blob_is_paired(const couple_store_blob_t *blob)
{
    if (blob == NULL) {
        return false;
    }
    if ((blob->flags & COUPLE_STORE_FLAG_PAIRED) == 0u) {
        return false;
    }
    return pair_trio_ok(blob);
}

uint32_t couple_store_blob_next_seq(couple_store_blob_t *blob)
{
    if (blob == NULL) {
        return 0u;
    }
    blob->sync_seq++;
    if (blob->sync_seq == 0u) {
        blob->sync_seq = 1u; /* 0 是「还没发过」的保留值，跳过 */
    }
    return blob->sync_seq;
}

void couple_store_blob_set_ack(couple_store_blob_t *blob, uint32_t ack)
{
    if (blob == NULL) {
        return;
    }
    if (ack > blob->sync_ack) {
        blob->sync_ack = ack;
    }
}

bool couple_store_blob_id_is_empty(const uint8_t *id, size_t len)
{
    return bytes_all_zero(id, len);
}

/* ---------- 字符串清洗 ---------- */

size_t couple_store_copy(char *out, size_t cap, const char *src)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (src == NULL) {
        return 0u;
    }

    size_t len = 0u;
    while (len + 1u < cap && src[len] != '\0') {
        out[len] = src[len];
        len++;
    }
    out[len] = '\0';
    return len;
}

size_t couple_store_sanitize_name(char *out, size_t cap, const char *src)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    out[0] = '\0';
    if (src == NULL) {
        return 0u;
    }

    const size_t total = strlen(src);
    size_t i = 0u;    /* src 游标 */
    size_t len = 0u;  /* 已经写进 out 的字节数 */
    size_t keep = 0u; /* 去掉尾部空白之后还有意义的长度 */

    while (i < total) {
        const unsigned char b = (unsigned char)src[i];

        /* 控制字符（含换行、制表、退格）一律丢掉，屏幕上也显示不出来。 */
        if (b < 0x20u || b == 0x7Fu) {
            i++;
            continue;
        }

        /* 空白只有 ASCII 空格留得下来：连续多个压成一个，开头不留。 */
        if (b == ' ') {
            if (len == 0u || out[len - 1u] == ' ') {
                i++;
                continue;
            }
            if (len + 1u >= cap) {
                break;
            }
            out[len++] = ' ';
            i++;
            continue;
        }

        const size_t seq = utf8_seq_len(src + i, total - i);
        if (seq == 0u) {
            break; /* 尾部残缺，宁可不要 */
        }
        if (len + seq >= cap) {
            break; /* 放不下整个字符就整个不写 */
        }
        for (size_t k = 0u; k < seq; ++k) {
            out[len + k] = src[i + k];
        }
        len += seq;
        keep = len;
        i += seq;
    }

    out[keep] = '\0';
    return keep;
}

bool couple_store_sanitize_relay_url(char *out, size_t cap, const char *src)
{
    if (out == NULL || cap == 0u) {
        return false;
    }
    out[0] = '\0';
    if (src == NULL) {
        return false;
    }

    bool https;
    size_t scheme_len;

    if (starts_with_ci(src, "https://", 8u)) {
        https = true;
        scheme_len = 8u;
    } else if (starts_with_ci(src, "http://", 7u)) {
        https = false;
        scheme_len = 7u;
    } else {
        return false; /* 只接受 http / https，方便用户一眼看出是明文还是加密 */
    }

    const size_t len = strlen(src);

    /*
     * 中间不允许出现空白或控制字符（那说明是把好几行粘进来了），
     * 也不允许带查询串或片段：这里存的是基础地址，路径由程序自己拼。
     */
    for (size_t i = scheme_len; i < len; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (c <= 0x20u || c == 0x7Fu || c == '?' || c == '#') {
            return false;
        }
    }

    size_t end = len;
    while (end > scheme_len && src[end - 1u] == '/') {
        end--;
    }
    if (end <= scheme_len) {
        return false; /* 只有 "http://" 或 "http:///"，没有主机名 */
    }

    const size_t host_len = end - scheme_len;
    if (scheme_len + host_len + 1u > cap) {
        return false; /* 放不下，不截断——截断出来的地址一定连不上 */
    }

    for (size_t i = 0u; i < scheme_len; ++i) {
        out[i] = https ? "https://"[i] : "http://"[i]; /* 统一小写，便于比较与日志 */
    }
    for (size_t i = 0u; i < host_len; ++i) {
        out[scheme_len + i] = src[scheme_len + i];
    }
    out[scheme_len + host_len] = '\0';
    return true;
}

/* ---------- 布局守卫 ---------- */

/*
 * 这份结构体会被原样写进 NVS，所以字节布局就是对外契约。
 * 任何一个字段改大小、换顺序、加字段，都会在这里编译失败 ——
 * 那是提醒你必须同时抬高 COUPLE_STORE_SCHEMA 并写好迁移策略，而不是改这个数字。
 */
_Static_assert(sizeof(couple_stats_t) == 60, "couple_stats_t 布局变化：请抬高 COUPLE_STORE_SCHEMA");
_Static_assert(sizeof(couple_store_blob_t) == 376,
               "couple_store_blob_t 布局变化：请抬高 COUPLE_STORE_SCHEMA");