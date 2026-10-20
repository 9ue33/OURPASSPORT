/*
 * SHA-256 / HMAC-SHA256 的纯 C 实现（FIPS 180-4 / RFC 2104）。
 *
 * 零动态分配：只用调用方给的缓冲和栈。状态全部放在 ctx 里，没有静态可变量，
 * 因此可以重入。正确性由 tests/test_couple_sha256.c 的 NIST / RFC 向量保证。
 */

#include "couple_sha256.h"

#include <string.h>

#define SHA256_BLOCK_BYTES 64u
#define SHA256_DIGEST_BYTES 32u
#define SHA256_IPAD 0x36u
#define SHA256_OPAD 0x5Cu

static const uint32_t sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static uint32_t rotr32(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static uint32_t load_be32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 8) |
           (uint32_t)in[3];
}

static void store_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void sha256_transform(couple_sha256_t *ctx, const uint8_t *block)
{
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    size_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = load_be32(block + (4u * i));
    }
    for (i = 16u; i < 64u; i++) {
        const uint32_t s0 = rotr32(w[i - 15u], 7) ^ rotr32(w[i - 15u], 18) ^ (w[i - 15u] >> 3);
        const uint32_t s1 = rotr32(w[i - 2u], 17) ^ rotr32(w[i - 2u], 19) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (i = 0; i < 64u; i++) {
        const uint32_t big_s1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t choose = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + big_s1 + choose + sha256_k[i] + w[i];
        const uint32_t big_s0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = big_s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void couple_sha256_init(couple_sha256_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bitlen = 0u;
    ctx->buffer_len = 0u;
    memset(ctx->buffer, 0, sizeof(ctx->buffer));
}

void couple_sha256_update(couple_sha256_t *ctx, const void *data, size_t len)
{
    const uint8_t *in = (const uint8_t *)data;

    if (ctx == NULL) {
        return;
    }
    if (len == 0u) {
        return;
    }
    if (in == NULL) {
        return;
    }

    /* 位数用 64 位累加，保证超过 2^32 位的流式输入也正确。 */
    ctx->bitlen += (uint64_t)len * 8u;

    if (ctx->buffer_len > 0u) {
        const size_t need = SHA256_BLOCK_BYTES - ctx->buffer_len;
        if (len < need) {
            memcpy(ctx->buffer + ctx->buffer_len, in, len);
            ctx->buffer_len += len;
            return;
        }
        memcpy(ctx->buffer + ctx->buffer_len, in, need);
        sha256_transform(ctx, ctx->buffer);
        ctx->buffer_len = 0u;
        in += need;
        len -= need;
    }

    while (len >= SHA256_BLOCK_BYTES) {
        sha256_transform(ctx, in);
        in += SHA256_BLOCK_BYTES;
        len -= SHA256_BLOCK_BYTES;
    }

    if (len > 0u) {
        memcpy(ctx->buffer, in, len);
        ctx->buffer_len = len;
    }
}

void couple_sha256_final(couple_sha256_t *ctx, uint8_t out[32])
{
    uint8_t padding[SHA256_BLOCK_BYTES];
    uint8_t length_be[8];
    uint64_t bitlen;
    size_t padding_len;
    size_t i;

    if (ctx == NULL || out == NULL) {
        return;
    }

    bitlen = ctx->bitlen;

    /* 0x80 之后补零，直到离块尾还剩 8 字节放长度。 */
    memset(padding, 0, sizeof(padding));
    padding[0] = 0x80u;
    if (ctx->buffer_len < 56u) {
        padding_len = 56u - ctx->buffer_len;
    } else {
        padding_len = 120u - ctx->buffer_len;
    }
    couple_sha256_update(ctx, padding, padding_len);

    for (i = 0; i < 8u; i++) {
        length_be[i] = (uint8_t)(bitlen >> (56u - (8u * i)));
    }
    couple_sha256_update(ctx, length_be, sizeof(length_be));

    for (i = 0; i < 8u; i++) {
        store_be32(out + (4u * i), ctx->state[i]);
    }
}

void couple_sha256(const void *data, size_t len, uint8_t out[32])
{
    couple_sha256_t ctx;

    if (out == NULL) {
        return;
    }
    couple_sha256_init(&ctx);
    couple_sha256_update(&ctx, data, len);
    couple_sha256_final(&ctx, out);
}

void couple_hmac_sha256(const uint8_t *key, size_t key_len, const void *data, size_t len,
                        uint8_t out[32])
{
    uint8_t padded_key[SHA256_BLOCK_BYTES];
    uint8_t ipad[SHA256_BLOCK_BYTES];
    uint8_t opad[SHA256_BLOCK_BYTES];
    uint8_t inner[SHA256_DIGEST_BYTES];
    couple_sha256_t ctx;
    size_t i;

    if (out == NULL) {
        return;
    }

    memset(padded_key, 0, sizeof(padded_key));
    if (key != NULL && key_len > 0u) {
        if (key_len > SHA256_BLOCK_BYTES) {
            /* 超长 key 先哈希成 32 字节，其余补零。 */
            couple_sha256(key, key_len, padded_key);
        } else {
            memcpy(padded_key, key, key_len);
        }
    }

    for (i = 0; i < SHA256_BLOCK_BYTES; i++) {
        ipad[i] = (uint8_t)(padded_key[i] ^ SHA256_IPAD);
        opad[i] = (uint8_t)(padded_key[i] ^ SHA256_OPAD);
    }

    couple_sha256_init(&ctx);
    couple_sha256_update(&ctx, ipad, sizeof(ipad));
    couple_sha256_update(&ctx, data, len);
    couple_sha256_final(&ctx, inner);

    couple_sha256_init(&ctx);
    couple_sha256_update(&ctx, opad, sizeof(opad));
    couple_sha256_update(&ctx, inner, sizeof(inner));
    couple_sha256_final(&ctx, out);

    memset(padded_key, 0, sizeof(padded_key));
    memset(ipad, 0, sizeof(ipad));
    memset(opad, 0, sizeof(opad));
    memset(inner, 0, sizeof(inner));
}

bool couple_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0u;
    size_t i;

    /*
     * 只有一个长度参数，「长度不等」只能由指针非法表达：指针为空即视为比较
     * 失败（fail closed），也避免解引用空指针。内容比较不做早退。
     */
    if (a == NULL || b == NULL) {
        return false;
    }

    for (i = 0; i < len; i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0u;
}