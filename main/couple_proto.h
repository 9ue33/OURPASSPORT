#pragma once

/*
 * Heartlink 情侣心动终端 —— 线协议 v1 编解码。
 *
 * 纯 C，不依赖 ESP-IDF，也不依赖 LVGL，可以在宿主机上直接编译和测试。
 * 所有多字节整数一律大端（网络序）。
 *
 * ============================ 帧格式 ============================
 *
 *   off 0   magic0 'H' (0x48)
 *   off 1   magic1 'L' (0x4C)
 *   off 2   version (1)
 *   off 3   type      (couple_frame_type_t)
 *   off 4   payload_len  u16 BE
 *   off 6   seq          u32 BE   发送方单调递增计数器，从 1 开始
 *   off 10  ack          u32 BE   已接受的对方最后一个 seq，没有则为 0
 *   off 14  payload[payload_len]
 *   末尾     hmac[16]              仅签名帧存在
 *
 *   hmac = HMAC-SHA256(shared_key, header || payload) 的前 16 字节
 *
 * 配对握手帧（0x01–0x06）在配对完成前还没有共享密钥，因此不带 HMAC，
 * 身份与配对码由 HTTP 层保证（一次性、短 TTL）。
 * 同步与解绑帧（0x07–0x0A）一律必须带 HMAC。
 *
 * ============================ 传输层 ============================
 *
 * 帧本身走 HTTP body（Content-Type: application/octet-stream），
 * 身份走 HTTP 头，不进帧：
 *   X-Device-Id   hex(16B)  所有请求
 *   X-Pair-Id     hex(8B)   SYNC / UNPAIR
 *   X-Pair-Token  hex(32B)  PAIR_AWAIT
 *
 *   POST /v1/pair/begin
 *   POST /v1/pair/claim
 *   POST /v1/pair/await
 *   POST /v1/sync
 *   POST /v1/unpair
 *
 * 设备侧因此完全不需要 JSON 解析器。
 * 所有 RSP 的 payload 首字节固定是 couple_status_t。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "couple_types.h"

/* ---------- 常量 ---------- */

#define COUPLE_PROTO_MAGIC0 0x48u /* 'H' */
#define COUPLE_PROTO_MAGIC1 0x4Cu /* 'L' */
#define COUPLE_PROTO_VERSION 1u

#define COUPLE_PROTO_HDR_LEN 14u
#define COUPLE_PROTO_HMAC_LEN 16u
#define COUPLE_PROTO_KEY_LEN 32u
#define COUPLE_PROTO_MAX_PAYLOAD 512u
#define COUPLE_PROTO_MAX_FRAME                                        \
    (COUPLE_PROTO_HDR_LEN + COUPLE_PROTO_MAX_PAYLOAD + COUPLE_PROTO_HMAC_LEN)

/* ---------- 帧头 ---------- */

typedef struct {
    uint8_t type; /* couple_frame_type_t */
    uint16_t payload_len;
    uint32_t seq;
    uint32_t ack;
} couple_frame_hdr_t;

/* 该帧类型是否带 HMAC（0x07–0x0A 为真，配对握手为假）。 */
bool couple_proto_frame_is_signed(uint8_t type);

/* 该帧完整长度（含 HMAC）。payload_len 超上限时返回 0。 */
size_t couple_proto_frame_len(uint8_t type, uint16_t payload_len);

/* 写 14 字节帧头。返回写入字节数，参数非法时返回 0。 */
size_t couple_proto_encode_header(uint8_t *out, size_t out_cap, uint8_t type,
                                  uint16_t payload_len, uint32_t seq, uint32_t ack);

/* 解析帧头。magic/version/payload_len 越界一律失败。 */
bool couple_proto_decode_header(const uint8_t *in, size_t len, couple_frame_hdr_t *out);

/*
 * 给已经写好的 header+payload 追加 HMAC。
 * frame 长度必须是 COUPLE_PROTO_HDR_LEN + payload_len。
 * 返回帧总长度；非签名帧或不支持的类型返回 0。
 */
size_t couple_proto_finalize(uint8_t *frame, uint8_t type, uint16_t payload_len,
                             const uint8_t key[COUPLE_PROTO_KEY_LEN]);

/* 校验完整帧并解析帧头。签名帧比对 HMAC，非签名帧只校验 magic/version/长度。 */
bool couple_proto_verify_frame(const uint8_t *frame, size_t len,
                               const uint8_t key[COUPLE_PROTO_KEY_LEN],
                               couple_frame_hdr_t *hdr_out);

/* ---------- 事件值辅助 ---------- */

void couple_event_set_u16(couple_event_t *ev, uint8_t type, uint16_t value);
bool couple_event_get_u16(const couple_event_t *ev, uint16_t *out);

/* ---------- 配对：PAIR_BEGIN ---------- */

/* payload: u8 name_len, name[utf8] */
size_t couple_proto_encode_pair_begin_req(uint8_t *out, size_t out_cap, const char *name);
bool couple_proto_decode_pair_begin_req(const uint8_t *payload, size_t len, char *name_out,
                                        size_t name_cap);

/* payload: u8 status, code[6] ASCII, u16 ttl_secs, token[32] */
typedef struct {
    uint8_t status;
    char code[COUPLE_PAIR_CODE_LEN + 1u];
    uint16_t ttl_secs;
    uint8_t token[COUPLE_PAIR_TOKEN_LEN];
} couple_pair_begin_rsp_t;

size_t couple_proto_encode_pair_begin_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_begin_rsp_t *rsp);
bool couple_proto_decode_pair_begin_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_begin_rsp_t *out);

/* ---------- 配对：PAIR_CLAIM ---------- */

/* payload: code[6], u8 name_len, name[utf8] */
size_t couple_proto_encode_pair_claim_req(uint8_t *out, size_t out_cap,
                                          const char code[COUPLE_PAIR_CODE_LEN], const char *name);

/* payload: u8 status, pair_id[8], key[32], partner_id[16], u8 name_len, name[utf8] */
typedef struct {
    uint8_t status;
    uint8_t pair_id[COUPLE_PAIR_ID_LEN];
    uint8_t key[COUPLE_PROTO_KEY_LEN];
    uint8_t partner_id[COUPLE_DEVICE_ID_LEN];
    char partner_name[COUPLE_NAME_MAX + 1u]; /* 已补 NUL */
} couple_pair_result_t;

size_t couple_proto_encode_pair_claim_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_result_t *rsp);
bool couple_proto_decode_pair_claim_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_result_t *out);

/* ---------- 配对：PAIR_AWAIT ---------- */

/* payload: 空 */
size_t couple_proto_encode_pair_await_req(uint8_t *out, size_t out_cap);

/* state 取值 */
#define COUPLE_PAIR_AWAIT_WAITING 0u
#define COUPLE_PAIR_AWAIT_PAIRED 1u
#define COUPLE_PAIR_AWAIT_EXPIRED 2u

/*
 * payload: u8 status, u8 state,
 *          state == PAIRED 时追加 pair_id[8], key[32], partner_id[16], u8 name_len, name
 */
typedef struct {
    uint8_t status;
    uint8_t state; /* COUPLE_PAIR_AWAIT_* */
    uint8_t pair_id[COUPLE_PAIR_ID_LEN];
    uint8_t key[COUPLE_PROTO_KEY_LEN];
    uint8_t partner_id[COUPLE_DEVICE_ID_LEN];
    char partner_name[COUPLE_NAME_MAX + 1u];
} couple_pair_await_rsp_t;

/* status == OK 且 state == PAIRED 且身份字段齐备时返回 true。 */
bool couple_pair_await_has_identity(const couple_pair_await_rsp_t *rsp);

size_t couple_proto_encode_pair_await_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_await_rsp_t *rsp);
bool couple_proto_decode_pair_await_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_await_rsp_t *out);

/* ---------- 同步 ---------- */

/* payload: u8 count, count * (u8 ev_type, u8 ev_len, ev_len bytes), u8 local_flags */
size_t couple_proto_encode_sync_req(uint8_t *out, size_t out_cap, const couple_event_list_t *events,
                                    uint8_t local_flags);
bool couple_proto_decode_sync_req(const uint8_t *payload, size_t len, couple_event_list_t *events,
                                  uint8_t *local_flags);

/*
 * payload: u8 status, [u8 count + 事件列表], u32 server_seq, u32 server_time,
 *          u8 partner_online, u8 partner_holding, u8 pair_state, u32 love_energy
 * love_energy 是 relay 权威的 pair Love Energy 总数，本机每次同步都用它覆盖显示值。
 * status != OK 时事件列表之后的字段一律不存在（语义不变）。
 * 注意：status == OK 时事件列表仍然存在（可能为空列表）。
 */
typedef struct {
    uint8_t status;
    couple_event_list_t events;
    uint32_t server_seq;
    uint32_t server_time; /* Unix 秒，UTC */
    uint8_t partner_online;  /* 0/1 */
    uint8_t partner_holding; /* 0/1，对方此刻是否按着 OK */
    uint8_t pair_state;      /* couple_pair_state_t */
    uint32_t love_energy;    /* pair 的 Love Energy 总数，relay 权威值 */
} couple_sync_rsp_t;

size_t couple_proto_encode_sync_rsp(uint8_t *out, size_t out_cap, const couple_sync_rsp_t *rsp);
bool couple_proto_decode_sync_rsp(const uint8_t *payload, size_t len, couple_sync_rsp_t *out);

/* ---------- 解绑 ---------- */

/* payload: 空 */
size_t couple_proto_encode_unpair_req(uint8_t *out, size_t out_cap);

/* payload: u8 status */
bool couple_proto_decode_unpair_rsp(const uint8_t *payload, size_t len, uint8_t *status_out);

/*
 * 供服务端/测试复用的通用 helper：写/读 "u8 status" 开头的最小响应。
 */
size_t couple_proto_encode_status_only(uint8_t *out, size_t out_cap, uint8_t status);