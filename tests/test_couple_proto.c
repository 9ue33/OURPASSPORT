/*
 * Heartlink 线协议 v1 编解码的宿主测试。线格式以 couple_proto.h 顶部注释为准。
 *
 * 覆盖：帧头编解码与各类拒绝、帧长/签名判定、签名帧 HMAC 收发（改 1 bit 必失败）、
 * 各 payload 的往返与非法输入拒绝、事件值辅助、缓冲区差 1 字节时编码器必须拒绝且不写入。
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "couple_proto.h"
#include "couple_sha256.h"
#include "couple_types.h"

static int g_failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                  \
            return 1;                                                      \
        }                                                                  \
    } while (0)

#define SENTINEL 0xA5u

static const uint8_t k_type_all[] = {
    COUPLE_FRAME_PAIR_BEGIN_REQ, COUPLE_FRAME_PAIR_BEGIN_RSP, COUPLE_FRAME_PAIR_CLAIM_REQ,
    COUPLE_FRAME_PAIR_CLAIM_RSP, COUPLE_FRAME_PAIR_AWAIT_REQ, COUPLE_FRAME_PAIR_AWAIT_RSP,
    COUPLE_FRAME_SYNC_REQ,       COUPLE_FRAME_SYNC_RSP,       COUPLE_FRAME_UNPAIR_REQ,
    COUPLE_FRAME_UNPAIR_RSP,
};

static void fill_key(uint8_t key[COUPLE_PROTO_KEY_LEN])
{
    size_t i;

    for (i = 0; i < COUPLE_PROTO_KEY_LEN; i++) {
        key[i] = (uint8_t)(0x10u + i);
    }
}

static void fill_sentinel(uint8_t *buf, size_t len, uint8_t value)
{
    memset(buf, value, len);
}

static int all_bytes_are(const uint8_t *buf, size_t len, uint8_t value)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* ---------- 帧头 ---------- */

static int test_header(void)
{
    uint8_t buf[COUPLE_PROTO_HDR_LEN + 4u];
    couple_frame_hdr_t hdr;
    size_t i;

    for (i = 0; i < sizeof(k_type_all); i++) {
        const uint8_t type = k_type_all[i];

        fill_sentinel(buf, sizeof(buf), SENTINEL);
        CHECK(couple_proto_encode_header(buf, sizeof(buf), type, 7u, 0x01020304u, 0x0A0B0C0Du) ==
              COUPLE_PROTO_HDR_LEN);
        CHECK(buf[0] == COUPLE_PROTO_MAGIC0);
        CHECK(buf[1] == COUPLE_PROTO_MAGIC1);
        CHECK(buf[2] == COUPLE_PROTO_VERSION);
        CHECK(buf[3] == type);
        CHECK(buf[4] == 0x00u && buf[5] == 0x07u); /* payload_len 大端 */
        CHECK(buf[6] == 0x01u && buf[7] == 0x02u && buf[8] == 0x03u && buf[9] == 0x04u);
        CHECK(buf[10] == 0x0Au && buf[11] == 0x0Bu && buf[12] == 0x0Cu && buf[13] == 0x0Du);
        CHECK(buf[14] == SENTINEL); /* 只写 14 字节 */

        memset(&hdr, 0, sizeof(hdr));
        CHECK(couple_proto_decode_header(buf, sizeof(buf), &hdr));
        CHECK(hdr.type == type);
        CHECK(hdr.payload_len == 7u);
        CHECK(hdr.seq == 0x01020304u);
        CHECK(hdr.ack == 0x0A0B0C0Du);
    }

    /* 参数非法 */
    CHECK(couple_proto_encode_header(NULL, 64u, COUPLE_FRAME_SYNC_REQ, 0u, 1u, 0u) == 0u);
    CHECK(couple_proto_encode_header(buf, COUPLE_PROTO_HDR_LEN - 1u, COUPLE_FRAME_SYNC_REQ, 0u, 1u,
                                     0u) == 0u);
    CHECK(couple_proto_encode_header(buf, sizeof(buf), 0x00u, 0u, 1u, 0u) == 0u); /* 未知类型 */
    CHECK(couple_proto_encode_header(buf, sizeof(buf), 0x0Bu, 0u, 1u, 0u) == 0u);
    CHECK(couple_proto_encode_header(buf, sizeof(buf), COUPLE_FRAME_SYNC_REQ,
                                     COUPLE_PROTO_MAX_PAYLOAD + 1u, 1u, 0u) == 0u);

    CHECK(couple_proto_encode_header(buf, sizeof(buf), COUPLE_FRAME_SYNC_REQ,
                                     COUPLE_PROTO_MAX_PAYLOAD, 1u, 0u) == COUPLE_PROTO_HDR_LEN);

    /* 解码拒绝 */
    CHECK(couple_proto_encode_header(buf, sizeof(buf), COUPLE_FRAME_SYNC_REQ, 4u, 1u, 0u) == 14u);
    CHECK(couple_proto_decode_header(buf, 14u, &hdr));
    CHECK(!couple_proto_decode_header(buf, 13u, &hdr)); /* 截断 */
    CHECK(!couple_proto_decode_header(NULL, 14u, &hdr));
    CHECK(!couple_proto_decode_header(buf, 14u, NULL));

    buf[0] = 0x47u; /* 坏 magic0 */
    CHECK(!couple_proto_decode_header(buf, 14u, &hdr));
    buf[0] = COUPLE_PROTO_MAGIC0;
    buf[1] = 0x4Du; /* 坏 magic1 */
    CHECK(!couple_proto_decode_header(buf, 14u, &hdr));
    buf[1] = COUPLE_PROTO_MAGIC1;
    buf[2] = 2u; /* 版本不对 */
    CHECK(!couple_proto_decode_header(buf, 14u, &hdr));
    buf[2] = COUPLE_PROTO_VERSION;
    buf[3] = 0x7Fu; /* 未知类型 */
    CHECK(!couple_proto_decode_header(buf, 14u, &hdr));
    buf[3] = COUPLE_FRAME_SYNC_REQ;
    buf[4] = 0x02u; /* payload_len = 0x0201 = 513 > 512 */
    buf[5] = 0x01u;
    CHECK(!couple_proto_decode_header(buf, 14u, &hdr));
    buf[4] = 0x02u;
    buf[5] = 0x00u; /* 512 合法 */
    CHECK(couple_proto_decode_header(buf, 14u, &hdr));
    CHECK(hdr.payload_len == COUPLE_PROTO_MAX_PAYLOAD);

    return 1;
}

/* ---------- 帧长与签名判定 ---------- */

static int test_frame_geometry(void)
{
    size_t i;

    for (i = 0; i < sizeof(k_type_all); i++) {
        const uint8_t type = k_type_all[i];
        const int signed_frame = (type >= COUPLE_FRAME_SYNC_REQ);
        const size_t want_len =
            COUPLE_PROTO_HDR_LEN + 10u +
            (signed_frame ? (size_t)COUPLE_PROTO_HMAC_LEN : (size_t)0u);

        CHECK(couple_proto_frame_is_signed(type) == (signed_frame != 0));
        CHECK(couple_proto_frame_len(type, 10u) == want_len);
        CHECK(couple_proto_frame_len(type, 0u) ==
              (size_t)COUPLE_PROTO_HDR_LEN +
                  (signed_frame ? (size_t)COUPLE_PROTO_HMAC_LEN : (size_t)0u));
        CHECK(couple_proto_frame_len(type, COUPLE_PROTO_MAX_PAYLOAD) ==
              COUPLE_PROTO_MAX_FRAME - (signed_frame ? 0u : (size_t)COUPLE_PROTO_HMAC_LEN));
        CHECK(couple_proto_frame_len(type, COUPLE_PROTO_MAX_PAYLOAD + 1u) == 0u);
    }

    /* 未知类型：一律不是签名帧，长度未知算 0。 */
    CHECK(!couple_proto_frame_is_signed(0x00u));
    CHECK(!couple_proto_frame_is_signed(0x0Bu));
    CHECK(!couple_proto_frame_is_signed(0xFFu));
    CHECK(couple_proto_frame_len(0x00u, 10u) == 0u);
    CHECK(couple_proto_frame_len(0x0Bu, 10u) == 0u);

    return 1;
}

/* ---------- 签名帧收发 ---------- */

static int test_signed_frame(void)
{
    uint8_t frame[COUPLE_PROTO_MAX_FRAME];
    uint8_t key[COUPLE_PROTO_KEY_LEN];
    uint8_t other_key[COUPLE_PROTO_KEY_LEN];
    uint8_t zerokey[COUPLE_PROTO_KEY_LEN];
    couple_event_list_t events;
    couple_frame_hdr_t hdr;
    uint16_t payload_len;
    size_t total;

    fill_key(key);
    fill_key(other_key);
    other_key[0] ^= 0x01u;
    memset(zerokey, 0, sizeof(zerokey));

    memset(&events, 0, sizeof(events));
    events.count = 2u;
    couple_event_set_u16(&events.items[0], COUPLE_EVENT_HUG_END, 1234u);
    events.items[1].type = COUPLE_EVENT_MISS;
    events.items[1].len = 0u;

    payload_len = (uint16_t)couple_proto_encode_sync_req(frame + COUPLE_PROTO_HDR_LEN,
                                                         sizeof(frame) - COUPLE_PROTO_HDR_LEN,
                                                         &events, COUPLE_SYNC_FLAG_HOLDING);
    CHECK(payload_len > 0u);
    CHECK(couple_proto_encode_header(frame, COUPLE_PROTO_HDR_LEN, COUPLE_FRAME_SYNC_REQ,
                                     payload_len, 9u, 4u) == COUPLE_PROTO_HDR_LEN);

    total = couple_proto_finalize(frame, COUPLE_FRAME_SYNC_REQ, payload_len, key);
    CHECK(total == (size_t)COUPLE_PROTO_HDR_LEN + payload_len + COUPLE_PROTO_HMAC_LEN);
    CHECK(couple_proto_frame_len(COUPLE_FRAME_SYNC_REQ, payload_len) == total);

    memset(&hdr, 0, sizeof(hdr));
    CHECK(couple_proto_verify_frame(frame, total, key, &hdr));
    CHECK(hdr.type == COUPLE_FRAME_SYNC_REQ);
    CHECK(hdr.payload_len == payload_len);
    CHECK(hdr.seq == 9u && hdr.ack == 4u);
    /* hdr_out 可以传 NULL。 */
    CHECK(couple_proto_verify_frame(frame, total, key, NULL));

    /* payload 改 1 bit */
    frame[COUPLE_PROTO_HDR_LEN] ^= 0x01u;
    CHECK(!couple_proto_verify_frame(frame, total, key, NULL));
    frame[COUPLE_PROTO_HDR_LEN] ^= 0x01u;
    CHECK(couple_proto_verify_frame(frame, total, key, NULL));

    /* MAC 改 1 bit（分别是首字节和末字节） */
    frame[total - 1u] ^= 0x80u;
    CHECK(!couple_proto_verify_frame(frame, total, key, NULL));
    frame[total - 1u] ^= 0x80u;
    frame[COUPLE_PROTO_HDR_LEN + payload_len] ^= 0x01u;
    CHECK(!couple_proto_verify_frame(frame, total, key, NULL));
    frame[COUPLE_PROTO_HDR_LEN + payload_len] ^= 0x01u;
    CHECK(couple_proto_verify_frame(frame, total, key, NULL));

    /* 帧头改 1 bit（seq 变了，HMAC 里的 header 也变了） */
    frame[6] ^= 0x01u;
    CHECK(!couple_proto_verify_frame(frame, total, key, NULL));
    frame[6] ^= 0x01u;
    CHECK(couple_proto_verify_frame(frame, total, key, NULL));

    /* 少 1 字节：签名帧必须 fail closed */
    CHECK(!couple_proto_verify_frame(frame, total - 1u, key, NULL));
    /* 换 key 也必须失败 */
    CHECK(!couple_proto_verify_frame(frame, total, other_key, NULL));
    /* 签名帧不给 key：失败 */
    CHECK(!couple_proto_verify_frame(frame, total, NULL, NULL));
    /* 截断到帧头：失败 */
    CHECK(!couple_proto_verify_frame(frame, COUPLE_PROTO_HDR_LEN, key, NULL));

    /* finalize 的非法输入 */
    CHECK(couple_proto_finalize(NULL, COUPLE_FRAME_SYNC_REQ, 0u, key) == 0u);
    CHECK(couple_proto_finalize(frame, COUPLE_FRAME_SYNC_REQ, 0u, NULL) == 0u);
    /* 非签名类型没有 HMAC 可加 */
    CHECK(couple_proto_finalize(frame, COUPLE_FRAME_PAIR_BEGIN_REQ, 0u, key) == 0u);
    CHECK(couple_proto_finalize(frame, 0x0Bu, 0u, key) == 0u);
    CHECK(couple_proto_finalize(frame, COUPLE_FRAME_SYNC_REQ, COUPLE_PROTO_MAX_PAYLOAD + 1u, key) ==
          0u);

    /* 非签名帧：任意 key 都能过 */
    payload_len = (uint16_t)couple_proto_encode_pair_begin_req(frame + COUPLE_PROTO_HDR_LEN,
                                                               sizeof(frame) - COUPLE_PROTO_HDR_LEN,
                                                               "Lin");
    CHECK(payload_len > 0u);
    CHECK(couple_proto_encode_header(frame, COUPLE_PROTO_HDR_LEN, COUPLE_FRAME_PAIR_BEGIN_REQ,
                                     payload_len, 1u, 0u) == COUPLE_PROTO_HDR_LEN);
    total = COUPLE_PROTO_HDR_LEN + (size_t)payload_len;
    CHECK(couple_proto_verify_frame(frame, total, zerokey, &hdr));
    CHECK(hdr.type == COUPLE_FRAME_PAIR_BEGIN_REQ);
    CHECK(couple_proto_verify_frame(frame, total, key, NULL));
    /* 非签名帧少 1 字节也要拒绝 */
    CHECK(!couple_proto_verify_frame(frame, total - 1u, zerokey, NULL));
    /* 末尾多出的字节不影响（帧可以嵌在更大的缓冲里） */
    CHECK(couple_proto_verify_frame(frame, total + 3u, zerokey, NULL));

    /* 未知类型整帧不可校验 */
    frame[3] = 0x0Bu;
    CHECK(!couple_proto_verify_frame(frame, total, zerokey, NULL));

    return 1;
}

/* ---------- PAIR_BEGIN ---------- */

static int test_pair_begin(void)
{
    uint8_t payload[COUPLE_PROTO_MAX_PAYLOAD];
    couple_pair_begin_rsp_t rsp;
    couple_pair_begin_rsp_t back;
    char name[COUPLE_NAME_MAX + 1u];
    char long_name[COUPLE_NAME_MAX + 2u];
    size_t n;
    size_t i;

    memset(name, 'N', sizeof(name) - 1u);
    name[sizeof(name) - 1u] = '\0';
    memset(long_name, 'L', sizeof(long_name) - 1u);
    long_name[sizeof(long_name) - 1u] = '\0';

    /* 普通名字往返 */
    n = couple_proto_encode_pair_begin_req(payload, sizeof(payload), "Lin");
    CHECK(n == 4u);
    CHECK(payload[0] == 3u);
    memset(name, 0, sizeof(name));
    CHECK(couple_proto_decode_pair_begin_req(payload, n, name, sizeof(name)));
    CHECK(strcmp(name, "Lin") == 0);

    /* 恰好 COUPLE_NAME_MAX 字节 */
    memset(name, 'N', (size_t)COUPLE_NAME_MAX);
    name[COUPLE_NAME_MAX] = '\0';
    n = couple_proto_encode_pair_begin_req(payload, sizeof(payload), name);
    CHECK(n == (size_t)COUPLE_NAME_MAX + 1u);
    CHECK(payload[0] == COUPLE_NAME_MAX);
    memset(name, 0, sizeof(name));
    CHECK(couple_proto_decode_pair_begin_req(payload, n, name, sizeof(name)));
    CHECK(strlen(name) == COUPLE_NAME_MAX);
    /* 名字缓冲不足时拒绝，而不是截断 */
    CHECK(!couple_proto_decode_pair_begin_req(payload, n, name, COUPLE_NAME_MAX));
    /* 名字超长则编码失败 */
    CHECK(couple_proto_encode_pair_begin_req(payload, sizeof(payload), long_name) == 0u);
    /* len 里的 name_len 超上限则解码失败 */
    payload[0] = (uint8_t)(COUPLE_NAME_MAX + 1u);
    CHECK(!couple_proto_decode_pair_begin_req(payload, n, name, sizeof(name)));
    payload[0] = COUPLE_NAME_MAX;
    CHECK(!couple_proto_decode_pair_begin_req(payload, n - 1u, name, sizeof(name)));
    CHECK(!couple_proto_decode_pair_begin_req(payload, 0u, name, sizeof(name)));
    CHECK(!couple_proto_decode_pair_begin_req(NULL, n, name, sizeof(name)));
    CHECK(!couple_proto_decode_pair_begin_req(payload, n, NULL, sizeof(name)));

    /* 空名字合法 */
    n = couple_proto_encode_pair_begin_req(payload, sizeof(payload), NULL);
    CHECK(n == 1u);
    CHECK(payload[0] == 0u);
    name[0] = 'x';
    CHECK(couple_proto_decode_pair_begin_req(payload, n, name, sizeof(name)));
    CHECK(name[0] == '\0');

    /* RSP 往返 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    memcpy(rsp.code, "A1B2C3", COUPLE_PAIR_CODE_LEN);
    rsp.ttl_secs = 600u;
    for (i = 0; i < COUPLE_PAIR_TOKEN_LEN; i++) {
        rsp.token[i] = (uint8_t)(0xC0u + i);
    }
    n = couple_proto_encode_pair_begin_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 1u + COUPLE_PAIR_CODE_LEN + 2u + COUPLE_PAIR_TOKEN_LEN);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_begin_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_OK);
    CHECK(strcmp(back.code, "A1B2C3") == 0);
    CHECK(back.ttl_secs == 600u);
    CHECK(memcmp(back.token, rsp.token, COUPLE_PAIR_TOKEN_LEN) == 0);
    CHECK(!couple_proto_decode_pair_begin_rsp(payload, n - 1u, &back));
    CHECK(!couple_proto_decode_pair_begin_rsp(NULL, n, &back));
    CHECK(!couple_proto_decode_pair_begin_rsp(payload, n, NULL));

    return 1;
}

/* ---------- PAIR_CLAIM ---------- */

static int test_pair_claim(void)
{
    uint8_t payload[COUPLE_PROTO_MAX_PAYLOAD];
    couple_pair_result_t rsp;
    couple_pair_result_t back;
    char request_name[COUPLE_NAME_MAX + 2u];
    size_t n;
    size_t i;

    memset(request_name, 'R', COUPLE_NAME_MAX);
    request_name[COUPLE_NAME_MAX] = '\0';

    n = couple_proto_encode_pair_claim_req(payload, sizeof(payload), "Q7W8E9", request_name);
    CHECK(n == COUPLE_PAIR_CODE_LEN + 1u + COUPLE_NAME_MAX);
    CHECK(memcmp(payload, "Q7W8E9", COUPLE_PAIR_CODE_LEN) == 0);
    CHECK(payload[COUPLE_PAIR_CODE_LEN] == COUPLE_NAME_MAX);
    CHECK(memcmp(payload + COUPLE_PAIR_CODE_LEN + 1u, request_name, COUPLE_NAME_MAX) == 0);
    CHECK(couple_proto_encode_pair_claim_req(payload, sizeof(payload), NULL, "x") == 0u);
    request_name[COUPLE_NAME_MAX] = 'R';
    request_name[COUPLE_NAME_MAX + 1u] = '\0';
    CHECK(couple_proto_encode_pair_claim_req(payload, sizeof(payload), "Q7W8E9", request_name) ==
          0u);

    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    for (i = 0; i < COUPLE_PAIR_ID_LEN; i++) {
        rsp.pair_id[i] = (uint8_t)(0x20u + i);
    }
    for (i = 0; i < COUPLE_PROTO_KEY_LEN; i++) {
        rsp.key[i] = (uint8_t)(0x40u + i);
    }
    for (i = 0; i < COUPLE_DEVICE_ID_LEN; i++) {
        rsp.partner_id[i] = (uint8_t)(0x60u + i);
    }
    strcpy(rsp.partner_name, "Xiao Yu");

    n = couple_proto_encode_pair_claim_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 1u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u +
                   strlen("Xiao Yu"));
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_claim_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_OK);
    CHECK(memcmp(back.pair_id, rsp.pair_id, COUPLE_PAIR_ID_LEN) == 0);
    CHECK(memcmp(back.key, rsp.key, COUPLE_PROTO_KEY_LEN) == 0);
    CHECK(memcmp(back.partner_id, rsp.partner_id, COUPLE_DEVICE_ID_LEN) == 0);
    CHECK(strcmp(back.partner_name, "Xiao Yu") == 0);

    /* 空名字：仍要补 NUL */
    strcpy(rsp.partner_name, "");
    n = couple_proto_encode_pair_claim_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 1u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_claim_rsp(payload, n, &back));
    CHECK(back.partner_name[0] == '\0');

    /* 超长名字拒绝 */
    /* 17 个 'Z' 把 17 字节填满、不留 NUL：长度超过 COUPLE_NAME_MAX，必须拒绝。
     * 不能写 partner_name[COUPLE_NAME_MAX + 1] —— 那是数组外一字节。 */
    memset(rsp.partner_name, 'Z', COUPLE_NAME_MAX + 1u);
    CHECK(couple_proto_encode_pair_claim_rsp(payload, sizeof(payload), &rsp) == 0u);

    /* 解码拒绝：短、name_len 越界、name 被截断 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    strcpy(rsp.partner_name, "May");
    n = couple_proto_encode_pair_claim_rsp(payload, sizeof(payload), &rsp);
    CHECK(!couple_proto_decode_pair_claim_rsp(payload, n - 1u, &back));
    CHECK(!couple_proto_decode_pair_claim_rsp(payload, 0u, &back));
    payload[n - 1u - 3u] = COUPLE_NAME_MAX + 1u; /* name_len 超上限 */
    CHECK(!couple_proto_decode_pair_claim_rsp(payload, n, &back));
    payload[n - 1u - 3u] = 3u;
    CHECK(couple_proto_decode_pair_claim_rsp(payload, n, &back));
    CHECK(!couple_proto_decode_pair_claim_rsp(NULL, n, &back));
    CHECK(!couple_proto_decode_pair_claim_rsp(payload, n, NULL));

    return 1;
}

/* ---------- PAIR_AWAIT ---------- */

static int test_pair_await(void)
{
    uint8_t payload[COUPLE_PROTO_MAX_PAYLOAD];
    couple_pair_await_rsp_t rsp;
    couple_pair_await_rsp_t back;
    size_t n;
    size_t i;

    CHECK(couple_proto_encode_pair_await_req(payload, sizeof(payload)) == 0u);
    CHECK(couple_proto_encode_pair_await_req(NULL, 0u) == 0u);

    /* WAITING / EXPIRED：没有身份字段 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    rsp.state = COUPLE_PAIR_AWAIT_WAITING;
    strcpy(rsp.partner_name, "ignored");
    n = couple_proto_encode_pair_await_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 2u);
    CHECK(payload[0] == COUPLE_STATUS_OK && payload[1] == COUPLE_PAIR_AWAIT_WAITING);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_await_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_OK);
    CHECK(back.state == COUPLE_PAIR_AWAIT_WAITING);
    CHECK(back.partner_name[0] == '\0');
    CHECK(!couple_pair_await_has_identity(&back));

    rsp.state = COUPLE_PAIR_AWAIT_EXPIRED;
    n = couple_proto_encode_pair_await_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 2u);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_await_rsp(payload, n, &back));
    CHECK(back.state == COUPLE_PAIR_AWAIT_EXPIRED);
    CHECK(!couple_pair_await_has_identity(&back));
    /* 非 PAIRED 状态的短载荷也要能解出来 */
    CHECK(couple_proto_decode_pair_await_rsp(payload, 2u, &back));

    /* PAIRED：身份字段齐备 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    rsp.state = COUPLE_PAIR_AWAIT_PAIRED;
    for (i = 0; i < COUPLE_PAIR_ID_LEN; i++) {
        rsp.pair_id[i] = (uint8_t)(0x11u + i);
    }
    for (i = 0; i < COUPLE_PROTO_KEY_LEN; i++) {
        rsp.key[i] = (uint8_t)(0x33u + i);
    }
    for (i = 0; i < COUPLE_DEVICE_ID_LEN; i++) {
        rsp.partner_id[i] = (uint8_t)(0x55u + i);
    }
    strcpy(rsp.partner_name, "Mei");
    n = couple_proto_encode_pair_await_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 2u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u + 3u);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_pair_await_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_OK);
    CHECK(back.state == COUPLE_PAIR_AWAIT_PAIRED);
    CHECK(memcmp(back.pair_id, rsp.pair_id, COUPLE_PAIR_ID_LEN) == 0);
    CHECK(memcmp(back.key, rsp.key, COUPLE_PROTO_KEY_LEN) == 0);
    CHECK(memcmp(back.partner_id, rsp.partner_id, COUPLE_DEVICE_ID_LEN) == 0);
    CHECK(strcmp(back.partner_name, "Mei") == 0);
    CHECK(couple_pair_await_has_identity(&back));

    /* PAIRED 身份字段被截断：解码失败，结构保持原样（未写入） */
    memset(&back, 0, sizeof(back));
    CHECK(!couple_proto_decode_pair_await_rsp(payload, n - 1u, &back));
    CHECK(!couple_pair_await_has_identity(&back));
    CHECK(!couple_proto_decode_pair_await_rsp(payload, 3u, &back));
    CHECK(!couple_proto_decode_pair_await_rsp(payload, 0u, &back));
    CHECK(!couple_proto_decode_pair_await_rsp(NULL, n, &back));
    CHECK(!couple_proto_decode_pair_await_rsp(payload, n, NULL));

    /* 有 PAIRED 状态但没有身份：has_identity 必须为假 */
    memset(&back, 0, sizeof(back));
    back.status = COUPLE_STATUS_OK;
    back.state = COUPLE_PAIR_AWAIT_PAIRED;
    CHECK(!couple_pair_await_has_identity(&back));
    CHECK(!couple_pair_await_has_identity(NULL));

    /* 身份齐备但 status != OK：仍然不算配好 */
    back.status = COUPLE_STATUS_CODE_EXPIRED;
    for (i = 0; i < COUPLE_PAIR_ID_LEN; i++) {
        back.pair_id[i] = 1u;
    }
    for (i = 0; i < COUPLE_PROTO_KEY_LEN; i++) {
        back.key[i] = 1u;
    }
    for (i = 0; i < COUPLE_DEVICE_ID_LEN; i++) {
        back.partner_id[i] = 1u;
    }
    CHECK(!couple_pair_await_has_identity(&back));
    back.status = COUPLE_STATUS_OK;
    CHECK(couple_pair_await_has_identity(&back));
    for (i = 0; i < COUPLE_DEVICE_ID_LEN; i++) {
        back.partner_id[i] = 0u;
    }
    CHECK(!couple_pair_await_has_identity(&back));

    /* name_len 超上限 */
    payload[2u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN] =
        COUPLE_NAME_MAX + 1u;
    CHECK(!couple_proto_decode_pair_await_rsp(payload, n, &back));

    return 1;
}

/* ---------- 事件值辅助与 SYNC_REQ ---------- */

static int test_event_helpers(void)
{
    couple_event_t ev;
    uint16_t value = 0u;

    memset(&ev, 0xFF, sizeof(ev));
    couple_event_set_u16(&ev, COUPLE_EVENT_HUG_END, 0x1234u);
    CHECK(ev.type == COUPLE_EVENT_HUG_END);
    CHECK(ev.len == 2u);
    CHECK(ev.value[0] == 0x12u && ev.value[1] == 0x34u); /* 大端 */
    CHECK(ev.value[2] == 0u);                            /* 其余清零 */
    CHECK(couple_event_get_u16(&ev, &value));
    CHECK(value == 0x1234u);

    couple_event_set_u16(&ev, COUPLE_EVENT_HEART_SYNC, 0u);
    CHECK(couple_event_get_u16(&ev, &value));
    CHECK(value == 0u);

    /* value 不足 2 字节：返回 false 且不写 out */
    ev.len = 1u;
    value = 0xABCDu;
    CHECK(!couple_event_get_u16(&ev, &value));
    CHECK(value == 0xABCDu);
    ev.len = 0u;
    CHECK(!couple_event_get_u16(&ev, &value));
    CHECK(!couple_event_get_u16(NULL, &value));
    CHECK(!couple_event_get_u16(&ev, NULL));
    couple_event_set_u16(NULL, COUPLE_EVENT_MISS, 1u);

    return 1;
}

static int test_sync_req(void)
{
    uint8_t payload[COUPLE_PROTO_MAX_PAYLOAD];
    couple_event_list_t events;
    couple_event_list_t back;
    uint8_t flags = 0u;
    size_t n;
    size_t i;

    /* 0 个事件 */
    memset(&events, 0, sizeof(events));
    n = couple_proto_encode_sync_req(payload, sizeof(payload), &events, 0u);
    CHECK(n == 2u);
    CHECK(payload[0] == 0u && payload[1] == 0u);
    memset(&back, 0xFF, sizeof(back));
    flags = 0xFFu;
    CHECK(couple_proto_decode_sync_req(payload, n, &back, &flags));
    CHECK(back.count == 0u && flags == 0u && back.flags == 0u);

    /* 1 个事件，value 里 len 之后的字节要清零 */
    memset(&events, 0, sizeof(events));
    events.count = 1u;
    events.items[0].type = COUPLE_EVENT_OFFLINE_SUMMARY;
    events.items[0].len = 3u;
    events.items[0].value[0] = 7u;
    events.items[0].value[1] = 8u;
    events.items[0].value[2] = 9u;
    for (i = 3u; i < COUPLE_EVENT_VALUE_MAX; i++) {
        events.items[0].value[i] = 0xFFu; /* 脏字节，不能上线 */
    }
    n = couple_proto_encode_sync_req(payload, sizeof(payload), &events, COUPLE_SYNC_FLAG_HOLDING);
    CHECK(n == 1u + 2u + 3u + 1u);
    memset(&back, 0, sizeof(back));
    flags = 0u;
    CHECK(couple_proto_decode_sync_req(payload, n, &back, &flags));
    CHECK(back.count == 1u);
    CHECK(back.items[0].type == COUPLE_EVENT_OFFLINE_SUMMARY);
    CHECK(back.items[0].len == 3u);
    CHECK(back.items[0].value[0] == 7u && back.items[0].value[2] == 9u);
    CHECK(all_bytes_are(back.items[0].value + 3u, COUPLE_EVENT_VALUE_MAX - 3u, 0u));
    CHECK(flags == COUPLE_SYNC_FLAG_HOLDING);
    CHECK(back.flags == COUPLE_SYNC_FLAG_HOLDING);

    /* 满列表 */
    memset(&events, 0, sizeof(events));
    events.count = COUPLE_EVENTS_PER_SYNC_MAX;
    for (i = 0; i < COUPLE_EVENTS_PER_SYNC_MAX; i++) {
        couple_event_set_u16(&events.items[i], COUPLE_EVENT_HUG_END, (uint16_t)(100u + i));
    }
    n = couple_proto_encode_sync_req(payload, sizeof(payload), &events, 0x01u);
    /* set_u16 的事件 len == 2，线上只写 2 个 value 字节。 */
    CHECK(n == 1u + (size_t)COUPLE_EVENTS_PER_SYNC_MAX * (2u + 2u) + 1u);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_sync_req(payload, n, &back, &flags));
    CHECK(back.count == COUPLE_EVENTS_PER_SYNC_MAX);
    for (i = 0; i < COUPLE_EVENTS_PER_SYNC_MAX; i++) {
        uint16_t value = 0u;
        CHECK(back.items[i].type == COUPLE_EVENT_HUG_END);
        CHECK(couple_event_get_u16(&back.items[i], &value));
        CHECK(value == (uint16_t)(100u + i));
    }

    /* count 超上限：编码拒绝、解码拒绝 */
    events.count = COUPLE_EVENTS_PER_SYNC_MAX + 1u;
    CHECK(couple_proto_encode_sync_req(payload, sizeof(payload), &events, 0u) == 0u);
    payload[0] = COUPLE_EVENTS_PER_SYNC_MAX + 1u;
    CHECK(!couple_proto_decode_sync_req(payload, 4u, &back, &flags));
    CHECK(couple_proto_encode_sync_req(payload, sizeof(payload), NULL, 0u) == 0u);

    /* 单个事件 len 超上限：编码拒绝 */
    memset(&events, 0, sizeof(events));
    events.count = 1u;
    events.items[0].type = COUPLE_EVENT_MISS;
    events.items[0].len = COUPLE_EVENT_VALUE_MAX + 1u;
    CHECK(couple_proto_encode_sync_req(payload, sizeof(payload), &events, 0u) == 0u);

    /* 事件自称的字节数超过实际携带：解码拒绝 */
    memset(payload, 0, sizeof(payload));
    payload[0] = 1u; /* count */
    payload[1] = COUPLE_EVENT_MISS;
    payload[2] = 5u; /* ev_len = 5，只给了 2 字节 */
    payload[3] = 0x01u;
    payload[4] = 0x02u;
    CHECK(!couple_proto_decode_sync_req(payload, 5u, &back, &flags));

    /* 事件 len 超上限：解码拒绝 */
    payload[2] = COUPLE_EVENT_VALUE_MAX + 1u;
    CHECK(!couple_proto_decode_sync_req(payload, 5u, &back, &flags));

    /* 缺 local_flags 字节：解码拒绝 */
    memset(&events, 0, sizeof(events));
    events.count = 1u;
    events.items[0].type = COUPLE_EVENT_MISS;
    n = couple_proto_encode_sync_req(payload, sizeof(payload), &events, 0u);
    CHECK(n == 1u + 2u + 1u); /* count + (type, len=0) + local_flags */
    CHECK(!couple_proto_decode_sync_req(payload, n - 1u, &back, &flags));
    CHECK(!couple_proto_decode_sync_req(payload, 0u, &back, &flags));
    CHECK(!couple_proto_decode_sync_req(NULL, n, &back, &flags));
    CHECK(!couple_proto_decode_sync_req(payload, n, NULL, &flags));
    CHECK(!couple_proto_decode_sync_req(payload, n, &back, NULL));

    return 1;
}

/* ---------- SYNC_RSP ---------- */

static int test_sync_rsp(void)
{
    uint8_t payload[COUPLE_PROTO_MAX_PAYLOAD];
    couple_sync_rsp_t rsp;
    couple_sync_rsp_t back;
    size_t n;
    size_t i;

    /* status == OK，空事件列表 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    rsp.server_seq = 0x11223344u;
    rsp.server_time = 1700000000u;
    rsp.partner_online = 1u;
    rsp.partner_holding = 1u;
    rsp.pair_state = COUPLE_PAIR_STATE_PAIRED;
    rsp.love_energy = 0x00000123u;
    n = couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 1u + 1u + 4u + 4u + 3u + 4u);
    memset(&back, 0, sizeof(back));
    CHECK(couple_proto_decode_sync_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_OK);
    CHECK(back.events.count == 0u);
    CHECK(back.server_seq == 0x11223344u);
    CHECK(back.server_time == 1700000000u);
    CHECK(back.partner_online == 1u && back.partner_holding == 1u);
    CHECK(back.pair_state == COUPLE_PAIR_STATE_PAIRED);
    CHECK(back.love_energy == 0x00000123u);

    /* status == OK，满事件列表 */
    memset(&back, 0, sizeof(back));
    rsp.events.count = COUPLE_EVENTS_PER_SYNC_MAX;
    for (i = 0; i < COUPLE_EVENTS_PER_SYNC_MAX; i++) {
        couple_event_set_u16(&rsp.events.items[i], COUPLE_EVENT_HUG_END, (uint16_t)(500u + i));
    }
    n = couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp);
    /* set_u16 的事件 len == 2。 */
    CHECK(n == 1u + 1u + (size_t)COUPLE_EVENTS_PER_SYNC_MAX * (2u + 2u) + 4u + 4u + 3u + 4u);
    CHECK(n <= COUPLE_PROTO_MAX_PAYLOAD);
    CHECK(couple_proto_decode_sync_rsp(payload, n, &back));
    CHECK(back.events.count == COUPLE_EVENTS_PER_SYNC_MAX);
    for (i = 0; i < COUPLE_EVENTS_PER_SYNC_MAX; i++) {
        uint16_t value = 0u;
        CHECK(couple_event_get_u16(&back.events.items[i], &value));
        CHECK(value == (uint16_t)(500u + i));
    }
    CHECK(back.server_seq == 0x11223344u && back.partner_online == 1u);

    /* status != OK：事件列表之后的字段不存在 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_PAIR_NOT_FOUND;
    rsp.server_seq = 0xDEADBEEFu;
    rsp.server_time = 0xDEADBEEFu;
    rsp.partner_online = 1u;
    rsp.partner_holding = 1u;
    rsp.pair_state = COUPLE_PAIR_STATE_PEER_LEFT;
    n = couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp);
    CHECK(n == 2u); /* status + count(=0) */
    CHECK(payload[0] == COUPLE_STATUS_PAIR_NOT_FOUND);
    CHECK(payload[1] == 0u);
    memset(&back, 0xFF, sizeof(back));
    CHECK(couple_proto_decode_sync_rsp(payload, n, &back));
    CHECK(back.status == COUPLE_STATUS_PAIR_NOT_FOUND);
    CHECK(back.partner_online == 0u);
    CHECK(back.partner_holding == 0u);
    CHECK(back.server_seq == 0u && back.server_time == 0u);
    CHECK(back.pair_state == 0u);
    CHECK(back.events.count == 0u);

    /* 只给 1 字节 status 的保守服务端实现也要能解 */
    payload[0] = COUPLE_STATUS_SERVER_BUSY;
    memset(&back, 0xFF, sizeof(back));
    CHECK(couple_proto_decode_sync_rsp(payload, 1u, &back));
    CHECK(back.status == COUPLE_STATUS_SERVER_BUSY);
    CHECK(back.partner_online == 0u && back.partner_holding == 0u);

    /* 非 OK 但事件列表不合法：拒绝 */
    payload[0] = COUPLE_STATUS_SERVER_BUSY;
    payload[1] = COUPLE_EVENTS_PER_SYNC_MAX + 1u;
    CHECK(!couple_proto_decode_sync_rsp(payload, 2u, &back));

    /* OK 路径的各种截断 */
    memset(&rsp, 0, sizeof(rsp));
    rsp.status = COUPLE_STATUS_OK;
    rsp.events.count = 1u;
    couple_event_set_u16(&rsp.events.items[0], COUPLE_EVENT_MISS, 3u);
    n = couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp);
    CHECK(couple_proto_decode_sync_rsp(payload, n, &back));
    /*
     * 帧尾是 love_energy 的 4 字节：n-1 / n-3 砍掉它的后 1 / 3 字节，
     * n-4 把它整段去掉——三种都必须解码失败（love_energy 是必需字段，
     * 不做可选尾随解析）。
     */
    CHECK(!couple_proto_decode_sync_rsp(payload, n - 1u, &back));
    CHECK(!couple_proto_decode_sync_rsp(payload, n - 3u, &back));
    CHECK(!couple_proto_decode_sync_rsp(payload, n - 4u, &back));
    CHECK(!couple_proto_decode_sync_rsp(payload, 1u, &back));
    CHECK(!couple_proto_decode_sync_rsp(payload, 0u, &back));
    CHECK(!couple_proto_decode_sync_rsp(NULL, n, &back));
    CHECK(!couple_proto_decode_sync_rsp(payload, n, NULL));
    CHECK(couple_proto_encode_sync_rsp(payload, sizeof(payload), NULL) == 0u);

    /* 事件非法时编码拒绝 */
    rsp.events.count = COUPLE_EVENTS_PER_SYNC_MAX + 1u;
    CHECK(couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp) == 0u);
    rsp.events.count = 1u;
    rsp.events.items[0].len = COUPLE_EVENT_VALUE_MAX + 1u;
    CHECK(couple_proto_encode_sync_rsp(payload, sizeof(payload), &rsp) == 0u);

    return 1;
}

/* ---------- 解绑与 status-only ---------- */

static int test_unpair(void)
{
    uint8_t payload[8];
    uint8_t status = 0xFFu;

    CHECK(couple_proto_encode_unpair_req(payload, sizeof(payload)) == 0u);
    CHECK(couple_proto_encode_unpair_req(NULL, 0u) == 0u);

    CHECK(couple_proto_encode_status_only(payload, sizeof(payload), COUPLE_STATUS_OK) == 1u);
    CHECK(payload[0] == COUPLE_STATUS_OK);
    CHECK(couple_proto_encode_status_only(NULL, 8u, COUPLE_STATUS_OK) == 0u);
    CHECK(couple_proto_encode_status_only(payload, 0u, COUPLE_STATUS_OK) == 0u);

    CHECK(couple_proto_decode_unpair_rsp(payload, 1u, &status));
    CHECK(status == COUPLE_STATUS_OK);
    status = 0xFFu;
    CHECK(!couple_proto_decode_unpair_rsp(payload, 0u, &status));
    CHECK(!couple_proto_decode_unpair_rsp(payload, 2u, &status)); /* 必须恰好 1 字节 */
    CHECK(!couple_proto_decode_unpair_rsp(NULL, 1u, &status));
    CHECK(!couple_proto_decode_unpair_rsp(payload, 1u, NULL));
    CHECK(status == 0xFFu); /* 失败时不动输出 */

    return 1;
}

/* ---------- 缓冲区差 1 字节：每个编码器都必须返回 0 且不写入 ---------- */

static int test_short_buffers(void)
{
    uint8_t big[COUPLE_PROTO_MAX_FRAME];
    uint8_t small[COUPLE_PROTO_MAX_FRAME];
    couple_event_list_t events;
    couple_pair_begin_rsp_t begin_rsp;
    couple_pair_result_t claim_rsp;
    couple_pair_await_rsp_t await_rsp;
    couple_sync_rsp_t sync_rsp;
    size_t n;

    memset(&events, 0, sizeof(events));
    events.count = 1u;
    couple_event_set_u16(&events.items[0], COUPLE_EVENT_HUG_END, 42u);

    memset(&begin_rsp, 0, sizeof(begin_rsp));
    memcpy(begin_rsp.code, "ABCDEF", COUPLE_PAIR_CODE_LEN);
    memset(&claim_rsp, 0, sizeof(claim_rsp));
    strcpy(claim_rsp.partner_name, "May");
    memset(&await_rsp, 0, sizeof(await_rsp));
    await_rsp.state = COUPLE_PAIR_AWAIT_PAIRED;
    strcpy(await_rsp.partner_name, "May");
    await_rsp.pair_id[0] = 1u;
    await_rsp.key[0] = 1u;
    await_rsp.partner_id[0] = 1u;
    memset(&sync_rsp, 0, sizeof(sync_rsp));
    sync_rsp.events.count = 1u;
    couple_event_set_u16(&sync_rsp.events.items[0], COUPLE_EVENT_MISS, 1u);

    /* 帧头 */
    CHECK(couple_proto_encode_header(big, sizeof(big), COUPLE_FRAME_SYNC_REQ, 4u, 1u, 0u) == 14u);
    fill_sentinel(small, sizeof(small), SENTINEL);
    CHECK(couple_proto_encode_header(small, 13u, COUPLE_FRAME_SYNC_REQ, 4u, 1u, 0u) == 0u);
    CHECK(all_bytes_are(small, sizeof(small), SENTINEL));

#define CHECK_ENCODER_TOO_SMALL(call_big, call_small)                   \
    do {                                                                \
        n = (call_big);                                                 \
        CHECK(n > 0u);                                                  \
        fill_sentinel(small, sizeof(small), SENTINEL);                   \
        CHECK((call_small) == 0u);                                       \
        CHECK(all_bytes_are(small, sizeof(small), SENTINEL));            \
    } while (0)

    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_pair_begin_req(big, sizeof(big), "Lin"),
        couple_proto_encode_pair_begin_req(small, n - 1u, "Lin"));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_pair_begin_rsp(big, sizeof(big), &begin_rsp),
        couple_proto_encode_pair_begin_rsp(small, n - 1u, &begin_rsp));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_pair_claim_req(big, sizeof(big), "ABCDEF", "Lin"),
        couple_proto_encode_pair_claim_req(small, n - 1u, "ABCDEF", "Lin"));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_pair_claim_rsp(big, sizeof(big), &claim_rsp),
        couple_proto_encode_pair_claim_rsp(small, n - 1u, &claim_rsp));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_pair_await_rsp(big, sizeof(big), &await_rsp),
        couple_proto_encode_pair_await_rsp(small, n - 1u, &await_rsp));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_sync_req(big, sizeof(big), &events, COUPLE_SYNC_FLAG_HOLDING),
        couple_proto_encode_sync_req(small, n - 1u, &events, COUPLE_SYNC_FLAG_HOLDING));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_sync_rsp(big, sizeof(big), &sync_rsp),
        couple_proto_encode_sync_rsp(small, n - 1u, &sync_rsp));
    CHECK_ENCODER_TOO_SMALL(
        couple_proto_encode_status_only(big, sizeof(big), COUPLE_STATUS_OK),
        couple_proto_encode_status_only(small, n - 1u, COUPLE_STATUS_OK));

#undef CHECK_ENCODER_TOO_SMALL

    /* 空载荷的两个编码器：不写任何字节，返回值就是 0。 */
    fill_sentinel(small, sizeof(small), SENTINEL);
    CHECK(couple_proto_encode_pair_await_req(small, sizeof(small)) == 0u);
    CHECK(couple_proto_encode_unpair_req(small, sizeof(small)) == 0u);
    CHECK(all_bytes_are(small, sizeof(small), SENTINEL));

    return 1;
}

int main(void)
{
    CHECK(test_header());
    CHECK(test_frame_geometry());
    CHECK(test_signed_frame());
    CHECK(test_event_helpers());
    CHECK(test_pair_begin());
    CHECK(test_pair_claim());
    CHECK(test_pair_await());
    CHECK(test_sync_req());
    CHECK(test_sync_rsp());
    CHECK(test_unpair());
    CHECK(test_short_buffers());

    if (g_failures != 0) {
        printf("couple_proto tests: FAIL (%d check(s))\n", g_failures);
        return 1;
    }
    puts("couple_proto tests: PASS");
    return 0;
}