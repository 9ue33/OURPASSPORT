/*
 * Heartlink 线协议 v1 编解码实现。线格式见 couple_proto.h 顶部注释。
 *
 * 纯 C、零动态分配、无静态可变量，可以在宿主机上编译测试，也可以被多个
 * 任务重入调用。所有多字节整数一律大端。
 */

#include "couple_proto.h"

#include <string.h>

#include "couple_sha256.h"

/* ---------- 基础读写 ---------- */

static uint16_t load_be16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8) | (uint16_t)in[1]);
}

static void store_be16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
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

/* ---------- 帧头与签名 ---------- */

static bool frame_type_known(uint8_t type)
{
    return type >= (uint8_t)COUPLE_FRAME_PAIR_BEGIN_REQ &&
           type <= (uint8_t)COUPLE_FRAME_UNPAIR_RSP;
}

bool couple_proto_frame_is_signed(uint8_t type)
{
    return type >= (uint8_t)COUPLE_FRAME_SYNC_REQ && type <= (uint8_t)COUPLE_FRAME_UNPAIR_RSP;
}

size_t couple_proto_frame_len(uint8_t type, uint16_t payload_len)
{
    if (!frame_type_known(type) || payload_len > COUPLE_PROTO_MAX_PAYLOAD) {
        return 0u;
    }
    return (size_t)COUPLE_PROTO_HDR_LEN + (size_t)payload_len +
           (couple_proto_frame_is_signed(type) ? (size_t)COUPLE_PROTO_HMAC_LEN : 0u);
}

size_t couple_proto_encode_header(uint8_t *out, size_t out_cap, uint8_t type, uint16_t payload_len,
                                  uint32_t seq, uint32_t ack)
{
    if (out == NULL || out_cap < COUPLE_PROTO_HDR_LEN) {
        return 0u;
    }
    if (!frame_type_known(type) || payload_len > COUPLE_PROTO_MAX_PAYLOAD) {
        return 0u;
    }

    out[0] = COUPLE_PROTO_MAGIC0;
    out[1] = COUPLE_PROTO_MAGIC1;
    out[2] = (uint8_t)COUPLE_PROTO_VERSION;
    out[3] = type;
    store_be16(out + 4, payload_len);
    store_be32(out + 6, seq);
    store_be32(out + 10, ack);
    return COUPLE_PROTO_HDR_LEN;
}

bool couple_proto_decode_header(const uint8_t *in, size_t len, couple_frame_hdr_t *out)
{
    uint16_t payload_len;

    if (in == NULL || out == NULL || len < COUPLE_PROTO_HDR_LEN) {
        return false;
    }
    if (in[0] != COUPLE_PROTO_MAGIC0 || in[1] != COUPLE_PROTO_MAGIC1) {
        return false;
    }
    if (in[2] != (uint8_t)COUPLE_PROTO_VERSION) {
        return false;
    }
    if (!frame_type_known(in[3])) {
        return false;
    }

    payload_len = load_be16(in + 4);
    if ((size_t)payload_len > COUPLE_PROTO_MAX_PAYLOAD) {
        return false;
    }

    out->type = in[3];
    out->payload_len = payload_len;
    out->seq = load_be32(in + 6);
    out->ack = load_be32(in + 10);
    return true;
}

size_t couple_proto_finalize(uint8_t *frame, uint8_t type, uint16_t payload_len,
                             const uint8_t key[COUPLE_PROTO_KEY_LEN])
{
    uint8_t mac[32];
    const size_t signed_len = (size_t)COUPLE_PROTO_HDR_LEN + (size_t)payload_len;

    if (frame == NULL || key == NULL) {
        return 0u;
    }
    if (!couple_proto_frame_is_signed(type) || payload_len > COUPLE_PROTO_MAX_PAYLOAD) {
        return 0u;
    }

    /* frame 目前必须已经包含 header + payload，共 signed_len 字节。 */
    couple_hmac_sha256(key, COUPLE_PROTO_KEY_LEN, frame, signed_len, mac);
    memcpy(frame + signed_len, mac, COUPLE_PROTO_HMAC_LEN);

    return signed_len + (size_t)COUPLE_PROTO_HMAC_LEN;
}

bool couple_proto_verify_frame(const uint8_t *frame, size_t len,
                               const uint8_t key[COUPLE_PROTO_KEY_LEN], couple_frame_hdr_t *hdr_out)
{
    couple_frame_hdr_t hdr;
    uint8_t mac[32];
    size_t total;
    size_t signed_len;

    if (frame == NULL) {
        return false;
    }
    if (!couple_proto_decode_header(frame, len, &hdr)) {
        return false;
    }

    total = couple_proto_frame_len(hdr.type, hdr.payload_len);
    if (total == 0u || len < total) {
        return false;
    }
    signed_len = (size_t)COUPLE_PROTO_HDR_LEN + (size_t)hdr.payload_len;

    if (couple_proto_frame_is_signed(hdr.type)) {
        if (key == NULL) {
            return false;
        }
        couple_hmac_sha256(key, COUPLE_PROTO_KEY_LEN, frame, signed_len, mac);
        if (!couple_ct_equal(mac, frame + signed_len, COUPLE_PROTO_HMAC_LEN)) {
            return false;
        }
    }

    if (hdr_out != NULL) {
        *hdr_out = hdr;
    }
    return true;
}

/* ---------- 事件值与事件列表 ---------- */

void couple_event_set_u16(couple_event_t *ev, uint8_t type, uint16_t value)
{
    if (ev == NULL) {
        return;
    }
    memset(ev->value, 0, sizeof(ev->value));
    ev->type = type;
    ev->len = 2u;
    ev->value[0] = (uint8_t)(value >> 8);
    ev->value[1] = (uint8_t)value;
}

bool couple_event_get_u16(const couple_event_t *ev, uint16_t *out)
{
    if (ev == NULL || out == NULL) {
        return false;
    }
    if (ev->len < 2u) {
        return false;
    }
    *out = (uint16_t)(((uint16_t)ev->value[0] << 8) | (uint16_t)ev->value[1]);
    return true;
}

static bool event_list_valid(const couple_event_list_t *list)
{
    size_t i;

    if (list == NULL) {
        return false;
    }
    if (list->count > COUPLE_EVENTS_PER_SYNC_MAX) {
        return false;
    }
    for (i = 0; i < (size_t)list->count; i++) {
        if (list->items[i].len > COUPLE_EVENT_VALUE_MAX) {
            return false;
        }
    }
    return true;
}

/* u8 count + count * (u8 type, u8 len, len bytes) */
static size_t event_list_wire_len(const couple_event_list_t *list)
{
    size_t total = 1u;
    size_t i;

    for (i = 0; i < (size_t)list->count; i++) {
        total += 2u + (size_t)list->items[i].len;
    }
    return total;
}

static void event_list_write(uint8_t *out, const couple_event_list_t *list)
{
    size_t i;

    *out++ = list->count;
    for (i = 0; i < (size_t)list->count; i++) {
        const couple_event_t *ev = &list->items[i];
        *out++ = ev->type;
        *out++ = ev->len;
        if (ev->len > 0u) {
            memcpy(out, ev->value, ev->len);
            out += ev->len;
        }
    }
}

/*
 * 读取事件列表。成功时 *consumed 为占用字节数并写出 *out（value 中 len 之后的
 * 字节清零，保证结构比较与 NVS 往返确定）。失败时不写 *out。
 */
static bool event_list_read(const uint8_t *in, size_t len, size_t *consumed,
                            couple_event_list_t *out)
{
    couple_event_list_t tmp;
    size_t pos = 1u;
    size_t i;
    uint8_t count;

    if (in == NULL || consumed == NULL || out == NULL || len < 1u) {
        return false;
    }

    count = in[0];
    if (count > COUPLE_EVENTS_PER_SYNC_MAX) {
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.count = count;

    for (i = 0; i < (size_t)count; i++) {
        uint8_t ev_len;

        if (len - pos < 2u) {
            return false;
        }
        ev_len = in[pos + 1u];
        if (ev_len > COUPLE_EVENT_VALUE_MAX) {
            return false;
        }
        if (len - pos - 2u < (size_t)ev_len) {
            return false;
        }
        tmp.items[i].type = in[pos];
        tmp.items[i].len = ev_len;
        if (ev_len > 0u) {
            memcpy(tmp.items[i].value, in + pos + 2u, ev_len);
        }
        pos += 2u + (size_t)ev_len;
    }

    *out = tmp;
    *consumed = pos;
    return true;
}

/* ---------- 配对：PAIR_BEGIN ---------- */

size_t couple_proto_encode_pair_begin_req(uint8_t *out, size_t out_cap, const char *name)
{
    size_t name_len = 0u;

    if (out == NULL) {
        return 0u;
    }
    if (name != NULL) {
        name_len = strlen(name);
    }
    if (name_len > COUPLE_NAME_MAX) {
        return 0u;
    }
    if (out_cap < name_len + 1u) {
        return 0u;
    }

    out[0] = (uint8_t)name_len;
    if (name_len > 0u) {
        memcpy(out + 1u, name, name_len);
    }
    return name_len + 1u;
}

bool couple_proto_decode_pair_begin_req(const uint8_t *payload, size_t len, char *name_out,
                                        size_t name_cap)
{
    size_t name_len;

    if (payload == NULL || name_out == NULL || len < 1u) {
        return false;
    }
    name_len = payload[0];
    if (name_len > COUPLE_NAME_MAX) {
        return false;
    }
    if (len < name_len + 1u) {
        return false;
    }
    if (name_cap < name_len + 1u) {
        return false;
    }

    if (name_len > 0u) {
        memcpy(name_out, payload + 1u, name_len);
    }
    name_out[name_len] = '\0';
    return true;
}

size_t couple_proto_encode_pair_begin_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_begin_rsp_t *rsp)
{
    const size_t need = 1u + COUPLE_PAIR_CODE_LEN + 2u + COUPLE_PAIR_TOKEN_LEN;

    if (out == NULL || rsp == NULL || out_cap < need) {
        return 0u;
    }

    out[0] = rsp->status;
    memcpy(out + 1u, rsp->code, COUPLE_PAIR_CODE_LEN);
    store_be16(out + 1u + COUPLE_PAIR_CODE_LEN, rsp->ttl_secs);
    memcpy(out + 1u + COUPLE_PAIR_CODE_LEN + 2u, rsp->token, COUPLE_PAIR_TOKEN_LEN);
    return need;
}

bool couple_proto_decode_pair_begin_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_begin_rsp_t *out)
{
    couple_pair_begin_rsp_t tmp;
    const size_t need = 1u + COUPLE_PAIR_CODE_LEN + 2u + COUPLE_PAIR_TOKEN_LEN;

    if (payload == NULL || out == NULL || len < need) {
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.status = payload[0];
    memcpy(tmp.code, payload + 1u, COUPLE_PAIR_CODE_LEN);
    tmp.code[COUPLE_PAIR_CODE_LEN] = '\0';
    tmp.ttl_secs = load_be16(payload + 1u + COUPLE_PAIR_CODE_LEN);
    memcpy(tmp.token, payload + 1u + COUPLE_PAIR_CODE_LEN + 2u, COUPLE_PAIR_TOKEN_LEN);

    *out = tmp;
    return true;
}

/* ---------- 配对：PAIR_CLAIM ---------- */

size_t couple_proto_encode_pair_claim_req(uint8_t *out, size_t out_cap,
                                          const char code[COUPLE_PAIR_CODE_LEN], const char *name)
{
    size_t name_len = 0u;

    if (out == NULL || code == NULL) {
        return 0u;
    }
    if (name != NULL) {
        name_len = strlen(name);
    }
    if (name_len > COUPLE_NAME_MAX) {
        return 0u;
    }
    if (out_cap < COUPLE_PAIR_CODE_LEN + 1u + name_len) {
        return 0u;
    }

    memcpy(out, code, COUPLE_PAIR_CODE_LEN);
    out[COUPLE_PAIR_CODE_LEN] = (uint8_t)name_len;
    if (name_len > 0u) {
        memcpy(out + COUPLE_PAIR_CODE_LEN + 1u, name, name_len);
    }
    return COUPLE_PAIR_CODE_LEN + 1u + name_len;
}

size_t couple_proto_encode_pair_claim_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_result_t *rsp)
{
    const size_t need_fixed =
        1u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u;
    size_t name_len = 0u;
    size_t need;
    uint8_t *p;

    if (out == NULL || rsp == NULL) {
        return 0u;
    }
    /* 有界求长：正常调用方保证 NUL 终止，但不拿未终止的输入去 strlen（会越界读）。 */
    while (name_len <= (size_t)COUPLE_NAME_MAX && rsp->partner_name[name_len] != '\0') {
        name_len++;
    }
    if (name_len > (size_t)COUPLE_NAME_MAX) {
        return 0u;
    }
    need = need_fixed + name_len;
    if (out_cap < need) {
        return 0u;
    }

    p = out;
    *p++ = rsp->status;
    memcpy(p, rsp->pair_id, COUPLE_PAIR_ID_LEN);
    p += COUPLE_PAIR_ID_LEN;
    memcpy(p, rsp->key, COUPLE_PROTO_KEY_LEN);
    p += COUPLE_PROTO_KEY_LEN;
    memcpy(p, rsp->partner_id, COUPLE_DEVICE_ID_LEN);
    p += COUPLE_DEVICE_ID_LEN;
    *p++ = (uint8_t)name_len;
    if (name_len > 0u) {
        memcpy(p, rsp->partner_name, name_len);
    }
    return need;
}

bool couple_proto_decode_pair_claim_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_result_t *out)
{
    const size_t need_fixed =
        1u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u;
    couple_pair_result_t tmp;
    size_t name_len;
    const uint8_t *p;

    if (payload == NULL || out == NULL || len < need_fixed) {
        return false;
    }

    name_len = payload[need_fixed - 1u];
    if (name_len > COUPLE_NAME_MAX) {
        return false;
    }
    if (len < need_fixed + name_len) {
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    p = payload;
    tmp.status = *p++;
    memcpy(tmp.pair_id, p, COUPLE_PAIR_ID_LEN);
    p += COUPLE_PAIR_ID_LEN;
    memcpy(tmp.key, p, COUPLE_PROTO_KEY_LEN);
    p += COUPLE_PROTO_KEY_LEN;
    memcpy(tmp.partner_id, p, COUPLE_DEVICE_ID_LEN);
    p += COUPLE_DEVICE_ID_LEN;
    p++; /* name_len，上面已读出 */
    if (name_len > 0u) {
        memcpy(tmp.partner_name, p, name_len);
    }
    tmp.partner_name[name_len] = '\0';

    *out = tmp;
    return true;
}

/* ---------- 配对：PAIR_AWAIT ---------- */

size_t couple_proto_encode_pair_await_req(uint8_t *out, size_t out_cap)
{
    /* 载荷为空：不写任何字节，长度为 0。 */
    (void)out;
    (void)out_cap;
    return 0u;
}

bool couple_pair_await_has_identity(const couple_pair_await_rsp_t *rsp)
{
    uint8_t pair_acc = 0u;
    uint8_t key_acc = 0u;
    uint8_t partner_acc = 0u;
    size_t i;

    if (rsp == NULL) {
        return false;
    }
    if (rsp->status != (uint8_t)COUPLE_STATUS_OK ||
        rsp->state != (uint8_t)COUPLE_PAIR_AWAIT_PAIRED) {
        return false;
    }

    /* 身份字段齐备 = 三段标识都不是全零（全零代表服务端没给全）。 */
    for (i = 0; i < COUPLE_PAIR_ID_LEN; i++) {
        pair_acc |= rsp->pair_id[i];
    }
    for (i = 0; i < COUPLE_PROTO_KEY_LEN; i++) {
        key_acc |= rsp->key[i];
    }
    for (i = 0; i < COUPLE_DEVICE_ID_LEN; i++) {
        partner_acc |= rsp->partner_id[i];
    }
    return pair_acc != 0u && key_acc != 0u && partner_acc != 0u;
}

size_t couple_proto_encode_pair_await_rsp(uint8_t *out, size_t out_cap,
                                          const couple_pair_await_rsp_t *rsp)
{
    size_t need = 2u;
    size_t name_len = 0u;
    uint8_t *p;

    if (out == NULL || rsp == NULL) {
        return 0u;
    }
    if (rsp->state == (uint8_t)COUPLE_PAIR_AWAIT_PAIRED) {
        name_len = strlen(rsp->partner_name);
        if (name_len > COUPLE_NAME_MAX) {
            return 0u;
        }
        need += COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u + name_len;
    }
    if (out_cap < need) {
        return 0u;
    }

    p = out;
    *p++ = rsp->status;
    *p++ = rsp->state;
    if (rsp->state == (uint8_t)COUPLE_PAIR_AWAIT_PAIRED) {
        memcpy(p, rsp->pair_id, COUPLE_PAIR_ID_LEN);
        p += COUPLE_PAIR_ID_LEN;
        memcpy(p, rsp->key, COUPLE_PROTO_KEY_LEN);
        p += COUPLE_PROTO_KEY_LEN;
        memcpy(p, rsp->partner_id, COUPLE_DEVICE_ID_LEN);
        p += COUPLE_DEVICE_ID_LEN;
        *p++ = (uint8_t)name_len;
        if (name_len > 0u) {
            memcpy(p, rsp->partner_name, name_len);
        }
    }
    return need;
}

bool couple_proto_decode_pair_await_rsp(const uint8_t *payload, size_t len,
                                        couple_pair_await_rsp_t *out)
{
    const size_t need_fixed =
        2u + COUPLE_PAIR_ID_LEN + COUPLE_PROTO_KEY_LEN + COUPLE_DEVICE_ID_LEN + 1u;
    couple_pair_await_rsp_t tmp;
    const uint8_t *p;

    if (payload == NULL || out == NULL || len < 2u) {
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.status = payload[0];
    tmp.state = payload[1];

    /* 身份字段只跟着 state == PAIRED 出现；其余状态一律留空。 */
    if (tmp.state == (uint8_t)COUPLE_PAIR_AWAIT_PAIRED) {
        size_t name_len;

        if (len < need_fixed) {
            return false;
        }
        name_len = payload[need_fixed - 1u];
        if (name_len > COUPLE_NAME_MAX) {
            return false;
        }
        if (len < need_fixed + name_len) {
            return false;
        }

        p = payload + 2u;
        memcpy(tmp.pair_id, p, COUPLE_PAIR_ID_LEN);
        p += COUPLE_PAIR_ID_LEN;
        memcpy(tmp.key, p, COUPLE_PROTO_KEY_LEN);
        p += COUPLE_PROTO_KEY_LEN;
        memcpy(tmp.partner_id, p, COUPLE_DEVICE_ID_LEN);
        p += COUPLE_DEVICE_ID_LEN;
        p++; /* name_len，上面已读出 */
        if (name_len > 0u) {
            memcpy(tmp.partner_name, p, name_len);
        }
        tmp.partner_name[name_len] = '\0';
    }

    *out = tmp;
    return true;
}

/* ---------- 同步 ---------- */

size_t couple_proto_encode_sync_req(uint8_t *out, size_t out_cap, const couple_event_list_t *events,
                                    uint8_t local_flags)
{
    size_t need;

    if (out == NULL || !event_list_valid(events)) {
        return 0u;
    }
    need = event_list_wire_len(events) + 1u;
    if (out_cap < need) {
        return 0u;
    }

    event_list_write(out, events);
    out[need - 1u] = local_flags;
    return need;
}

bool couple_proto_decode_sync_req(const uint8_t *payload, size_t len, couple_event_list_t *events,
                                  uint8_t *local_flags)
{
    couple_event_list_t tmp;
    size_t consumed;

    if (payload == NULL || events == NULL || local_flags == NULL) {
        return false;
    }
    if (!event_list_read(payload, len, &consumed, &tmp)) {
        return false;
    }
    if (len - consumed < 1u) {
        return false;
    }

    tmp.flags = payload[consumed];
    *events = tmp;
    *local_flags = tmp.flags;
    return true;
}

size_t couple_proto_encode_sync_rsp(uint8_t *out, size_t out_cap, const couple_sync_rsp_t *rsp)
{
    size_t need;
    size_t list_len;
    uint8_t *p;

    if (out == NULL || rsp == NULL || !event_list_valid(&rsp->events)) {
        return 0u;
    }

    list_len = event_list_wire_len(&rsp->events);
    /* status != OK 时，事件列表之后的字段一律不存在（事件列表本身仍在）。 */
    need = 1u + list_len;
    if (rsp->status == (uint8_t)COUPLE_STATUS_OK) {
        need += 4u + 4u + 1u + 1u + 1u + 4u;
    }
    if (out_cap < need) {
        return 0u;
    }

    p = out;
    *p++ = rsp->status;
    event_list_write(p, &rsp->events);
    p += list_len;
    if (rsp->status == (uint8_t)COUPLE_STATUS_OK) {
        store_be32(p, rsp->server_seq);
        p += 4u;
        store_be32(p, rsp->server_time);
        p += 4u;
        *p++ = rsp->partner_online;
        *p++ = rsp->partner_holding;
        *p++ = rsp->pair_state;
        store_be32(p, rsp->love_energy);
        p += 4u;
    }
    return need;
}

bool couple_proto_decode_sync_rsp(const uint8_t *payload, size_t len, couple_sync_rsp_t *out)
{
    couple_sync_rsp_t tmp;
    couple_event_list_t list;
    size_t consumed = 0u;
    size_t pos = 1u;

    if (payload == NULL || out == NULL || len < 1u) {
        return false;
    }

    memset(&tmp, 0, sizeof(tmp));
    tmp.status = payload[0];

    if (tmp.status != (uint8_t)COUPLE_STATUS_OK) {
        /*
         * 非 OK：事件列表之后的所有字段缺失。事件列表本身按规范仍在，
         * 但只给 1 字节 status 的实现也认（此时视为空列表），避免对端保守实现解码失败。
         */
        if (len > pos) {
            if (!event_list_read(payload + pos, len - pos, &consumed, &list)) {
                return false;
            }
            tmp.events = list;
        }
        *out = tmp;
        return true;
    }

    if (!event_list_read(payload + pos, len - pos, &consumed, &list)) {
        return false;
    }
    tmp.events = list;
    pos += consumed;

    if (len - pos < 4u) {
        return false;
    }
    tmp.server_seq = load_be32(payload + pos);
    pos += 4u;

    if (len - pos < 4u) {
        return false;
    }
    tmp.server_time = load_be32(payload + pos);
    pos += 4u;

    if (len - pos < 3u) {
        return false;
    }
    tmp.partner_online = payload[pos];
    tmp.partner_holding = payload[pos + 1u];
    tmp.pair_state = payload[pos + 2u];
    pos += 3u;

    /*
     * love_energy 是必需的 4 字节，不做可选尾随解析：relay 一定会带（它先部署），
     * 缺了就是这一帧不完整，按解码失败处理。尾随多余字节照旧忽略，所以「新 relay
     * + 旧固件」也仍然能工作。
     */
    if (len - pos < 4u) {
        return false;
    }
    tmp.love_energy = load_be32(payload + pos);

    *out = tmp;
    return true;
}

/* ---------- 解绑 ---------- */

size_t couple_proto_encode_unpair_req(uint8_t *out, size_t out_cap)
{
    /* 载荷为空：不写任何字节，长度为 0。 */
    (void)out;
    (void)out_cap;
    return 0u;
}

bool couple_proto_decode_unpair_rsp(const uint8_t *payload, size_t len, uint8_t *status_out)
{
    if (payload == NULL || status_out == NULL || len != 1u) {
        return false;
    }
    *status_out = payload[0];
    return true;
}

size_t couple_proto_encode_status_only(uint8_t *out, size_t out_cap, uint8_t status)
{
    if (out == NULL || out_cap < 1u) {
        return 0u;
    }
    out[0] = status;
    return 1u;
}