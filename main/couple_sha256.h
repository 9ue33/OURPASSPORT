#pragma once

/*
 * 纯 C 的 SHA-256 / HMAC-SHA256（FIPS 180-4 / RFC 2104）。
 *
 * 为什么自己写：协议编解码要能在宿主上用 gcc 直接跑测试，而不引入 ESP-IDF 或
 * mbedtls；同时这条路零动态内存占用，适合 ESP32-C3 无 PSRAM 的处境。
 * 正确性由 tests/test_couple_sha256.c 中的 NIST / RFC 向量保证。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buffer[64];
    size_t buffer_len;
} couple_sha256_t;

void couple_sha256_init(couple_sha256_t *ctx);
void couple_sha256_update(couple_sha256_t *ctx, const void *data, size_t len);
/* 写出后 ctx 视为已失效，需重新 init 才能复用。 */
void couple_sha256_final(couple_sha256_t *ctx, uint8_t out[32]);

/* 一步式摘要。out 必须指向 32 字节。 */
void couple_sha256(const void *data, size_t len, uint8_t out[32]);

/* HMAC-SHA256。out 必须指向 32 字节。key 可为 NULL 当 key_len == 0。 */
void couple_hmac_sha256(const uint8_t *key, size_t key_len,
                        const void *data, size_t len, uint8_t out[32]);

/* 常量时间比较，用于校验 HMAC，避免早退泄露。长度不等直接返回 false。 */
bool couple_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);