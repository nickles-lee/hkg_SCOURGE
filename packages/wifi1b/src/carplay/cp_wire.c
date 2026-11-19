/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Independent C11 implementation of the bounded CSM/TLV codec described in
 * cp_wire.h.  No dynamic allocation, no I/O, no logging.
 */

#include "cp_wire.h"

#include <string.h>

#define CP_TLV_MAX_ID 31u

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t tlv_pos[CP_WIRE_MAX_TLV_DEPTH];
    size_t depth;
    cp_wire_status error;
} cp_writer;

/* These lists are the complete set of messages this checkpoint implements.
 * Do not copy a larger reference set: declaring a message the accessory does
 * not handle is dishonest and would invite traffic the state machine cannot
 * safely process. */
static const uint16_t k_accessory_messages[] = {
    CP_WIRE_MSG_IDENTIFICATION_INFO,
    CP_WIRE_MSG_AUTH_CERTIFICATE,
    CP_WIRE_MSG_AUTH_RESPONSE,
    CP_WIRE_MSG_WIFI_CONFIG_INFO,
    CP_WIRE_MSG_CARPLAY_START,
};

static const uint16_t k_phone_messages[] = {
    CP_WIRE_MSG_IDENTIFICATION_START,
    CP_WIRE_MSG_IDENTIFICATION_ACCEPTED,
    CP_WIRE_MSG_IDENTIFICATION_REJECTED,
    CP_WIRE_MSG_AUTH_CERT_REQUEST,
    CP_WIRE_MSG_AUTH_CHALLENGE_REQUEST,
    CP_WIRE_MSG_AUTH_FAILED,
    CP_WIRE_MSG_AUTH_SUCCEEDED,
    CP_WIRE_MSG_WIFI_CONFIG_REQUEST,
    CP_WIRE_MSG_CARPLAY_AVAILABILITY,
    CP_WIRE_MSG_WIRELESS_UPDATE,
    CP_WIRE_MSG_TRANSPORT_IDS,
};

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static size_t bounded_strlen(const char *s, size_t limit)
{
    size_t n = 0;
    if (s == NULL) {
        return limit + 1u;
    }
    while (n <= limit && s[n] != '\0') {
        n++;
    }
    return n;
}

bool cp_wire_utf8_valid(const uint8_t *data, size_t len)
{
    size_t i = 0;

    if (data == NULL && len != 0u) {
        return false;
    }
    while (i < len) {
        uint8_t c = data[i++];

        if (c == 0u) {
            return false;
        }
        if (c < 0x80u) {
            continue;
        }
        if (c < 0xC2u) {
            return false;
        }
        if (c < 0xE0u) {
            if (i >= len || (data[i] & 0xC0u) != 0x80u) {
                return false;
            }
            i++;
        } else if (c < 0xF0u) {
            uint8_t c1;
            uint8_t c2;
            if (i + 1u >= len) {
                return false;
            }
            c1 = data[i];
            c2 = data[i + 1u];
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) {
                return false;
            }
            if (c == 0xE0u && c1 < 0xA0u) {
                return false;
            }
            if (c == 0xEDu && c1 >= 0xA0u) {
                return false;
            }
            i += 2u;
        } else if (c < 0xF5u) {
            uint8_t c1;
            uint8_t c2;
            uint8_t c3;
            if (i + 2u >= len) {
                return false;
            }
            c1 = data[i];
            c2 = data[i + 1u];
            c3 = data[i + 2u];
            if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u ||
                (c3 & 0xC0u) != 0x80u) {
                return false;
            }
            if (c == 0xF0u && c1 < 0x90u) {
                return false;
            }
            if (c == 0xF4u && c1 >= 0x90u) {
                return false;
            }
            i += 3u;
        } else {
            return false;
        }
    }
    return true;
}

static bool text_valid(const uint8_t *data, size_t len)
{
    size_t i;

    if (data == NULL) {
        return len == 0u;
    }
    for (i = 0; i < len; i++) {
        if (data[i] == 0u) {
            return false;
        }
    }
    return cp_wire_utf8_valid(data, len);
}

static cp_wire_status string_payload(const uint8_t *payload, size_t len,
                                     size_t max_len, bool allow_empty)
{
    size_t text_len;

    if (payload == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (len == 0u) {
        /* Optional strings may be represented by an absent value.  Required
         * callers pass allow_empty == false and therefore reject this. */
        return allow_empty ? CP_WIRE_OK : CP_WIRE_ERR_MISSING;
    }
    if (payload[len - 1u] != 0u) {
        return CP_WIRE_ERR_UTF8;
    }
    text_len = len - 1u;
    if (text_len > max_len) {
        return CP_WIRE_ERR_RANGE;
    }
    if (text_len == 0u && !allow_empty) {
        return CP_WIRE_ERR_RANGE;
    }
    if (!text_valid(payload, text_len)) {
        return CP_WIRE_ERR_UTF8;
    }
    return CP_WIRE_OK;
}

static cp_wire_status copy_string_payload(const uint8_t *payload, size_t len,
                                          size_t max_len, bool allow_empty,
                                          char *out, size_t out_cap)
{
    size_t n;
    cp_wire_status st;

    if (out == NULL || out_cap == 0u) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    st = string_payload(payload, len, max_len, allow_empty);
    if (st != CP_WIRE_OK) {
        return st;
    }
    n = (len == 0u) ? 0u : len - 1u;
    if (n + 1u > out_cap) {
        return CP_WIRE_ERR_BUFFER;
    }
    if (n != 0u) {
        memcpy(out, payload, n);
    }
    out[n] = '\0';
    return CP_WIRE_OK;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool cp_wire_bt_mac_usable(const uint8_t mac[6])
{
    size_t i;
    bool any = false;
    bool all_ff = true;

    if (mac == NULL) {
        return false;
    }
    if ((mac[0] & 0x01u) != 0u) {
        return false;
    }
    for (i = 0; i < 6u; i++) {
        if (mac[i] != 0u) {
            any = true;
        }
        if (mac[i] != 0xFFu) {
            all_ff = false;
        }
    }
    return any && !all_ff;
}

cp_wire_status cp_wire_parse_bt_mac(const char *text, uint8_t out[6])
{
    size_t i;

    if (text == NULL || out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (bounded_strlen(text, 17u) != 17u) {
        return CP_WIRE_ERR_RANGE;
    }
    for (i = 0; i < 6u; i++) {
        int hi;
        int lo;
        size_t pos = i * 3u;
        if (i != 0u && text[pos - 1u] != ':') {
            return CP_WIRE_ERR_RANGE;
        }
        hi = hex_nibble(text[pos]);
        lo = hex_nibble(text[pos + 1u]);
        if (hi < 0 || lo < 0) {
            return CP_WIRE_ERR_RANGE;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    if (text[17] != '\0') {
        return CP_WIRE_ERR_RANGE;
    }
    return cp_wire_bt_mac_usable(out) ? CP_WIRE_OK : CP_WIRE_ERR_RANGE;
}

bool cp_wire_link_local_usable(const char *text)
{
    uint16_t groups[8];
    size_t group_count = 0;
    size_t i = 0;
    size_t len;
    bool compressed = false;
    bool lower_nonzero = false;

    if (text == NULL) {
        return false;
    }
    len = bounded_strlen(text, CP_WIRE_MAX_LINK_LOCAL);
    if (len < 4u || len > CP_WIRE_MAX_LINK_LOCAL) {
        return false;
    }

    if (text[0] == ':') {
        /* Leading compression makes the actual first word zero, regardless
         * of the first explicit group: ::fe80:1 is not a link-local address. */
        return false;
    }

    while (i < len) {
        uint16_t value = 0;
        size_t digits = 0;

        while (i < len && text[i] != ':') {
            int digit = hex_nibble(text[i]);
            if (digit < 0 || digits >= 4u) {
                return false;
            }
            value = (uint16_t)((value << 4) | (uint16_t)digit);
            digits++;
            i++;
        }
        if (digits == 0u || group_count >= 8u) {
            return false;
        }
        groups[group_count++] = value;

        if (i == len) {
            break;
        }
        if (text[i] != ':') {
            return false;
        }
        if (i + 1u < len && text[i + 1u] == ':') {
            if (compressed) {
                return false;
            }
            compressed = true;
            i += 2u;
            if (i == len) {
                break;
            }
        } else {
            i++;
            if (i == len) {
                return false; /* a single trailing colon is malformed */
            }
        }
    }

    if (group_count == 0u) {
        return false;
    }
    if (compressed) {
        if (group_count >= 8u) {
            return false;
        }
    } else if (group_count != 8u) {
        return false;
    }
    if (groups[0] < 0xFE80u || groups[0] > 0xFEBFu) {
        return false;
    }
    for (i = 1; i < group_count; i++) {
        if (groups[i] != 0u) {
            lower_nonzero = true;
        }
    }
    return lower_nonzero;
}

static void writer_init(cp_writer *w, uint8_t *out, size_t cap)
{
    w->buf = out;
    w->cap = cap;
    w->len = 0;
    w->depth = 0;
    w->error = CP_WIRE_OK;
}

static bool writer_need(cp_writer *w, size_t n)
{
    if (w->error != CP_WIRE_OK) {
        return false;
    }
    if (w->buf == NULL || w->cap < w->len || n > w->cap - w->len) {
        w->error = CP_WIRE_ERR_BUFFER;
        return false;
    }
    return true;
}

static void writer_u8(cp_writer *w, uint8_t value)
{
    if (writer_need(w, 1u)) {
        w->buf[w->len++] = value;
    }
}

static void writer_be16(cp_writer *w, uint16_t value)
{
    if (writer_need(w, 2u)) {
        w->buf[w->len++] = (uint8_t)(value >> 8);
        w->buf[w->len++] = (uint8_t)value;
    }
}

static void writer_be32(cp_writer *w, uint32_t value)
{
    if (writer_need(w, 4u)) {
        w->buf[w->len++] = (uint8_t)(value >> 24);
        w->buf[w->len++] = (uint8_t)(value >> 16);
        w->buf[w->len++] = (uint8_t)(value >> 8);
        w->buf[w->len++] = (uint8_t)value;
    }
}

static void writer_bytes(cp_writer *w, const uint8_t *data, size_t len)
{
    if (len == 0u) {
        return;
    }
    if (data == NULL) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    if (writer_need(w, len)) {
        memcpy(w->buf + w->len, data, len);
        w->len += len;
    }
}

static void writer_text(cp_writer *w, const char *text, size_t min_len,
                        size_t max_len)
{
    size_t n;
    size_t i;

    if (w->error != CP_WIRE_OK) {
        return;
    }
    if (text == NULL) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    n = bounded_strlen(text, max_len);
    if (n > max_len || n < min_len) {
        w->error = CP_WIRE_ERR_RANGE;
        return;
    }
    for (i = 0; i < n; i++) {
        if ((uint8_t)text[i] == 0u) {
            w->error = CP_WIRE_ERR_UTF8;
            return;
        }
    }
    if (!cp_wire_utf8_valid((const uint8_t *)text, n)) {
        w->error = CP_WIRE_ERR_UTF8;
        return;
    }
    writer_bytes(w, (const uint8_t *)text, n);
    writer_u8(w, 0u);
}

static void writer_frame_begin(cp_writer *w, uint16_t msg_id)
{
    if (w->error != CP_WIRE_OK) {
        return;
    }
    if (w->len != 0u || w->depth != 0u) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    writer_be16(w, CP_WIRE_CSM_START);
    writer_be16(w, 0u);
    writer_be16(w, msg_id);
}

static void writer_tlv_begin(cp_writer *w, uint16_t id)
{
    if (w->error != CP_WIRE_OK) {
        return;
    }
    if (w->depth >= CP_WIRE_MAX_TLV_DEPTH) {
        w->error = CP_WIRE_ERR_BUFFER;
        return;
    }
    if (!writer_need(w, CP_WIRE_TLV_HEADER_LEN)) {
        return;
    }
    w->tlv_pos[w->depth++] = w->len;
    writer_be16(w, 0u);
    writer_be16(w, id);
}

static void writer_tlv_end(cp_writer *w)
{
    size_t pos;
    size_t payload_len;
    size_t total;

    if (w->error != CP_WIRE_OK) {
        return;
    }
    if (w->depth == 0u) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    pos = w->tlv_pos[--w->depth];
    payload_len = w->len - pos - CP_WIRE_TLV_HEADER_LEN;
    total = payload_len + CP_WIRE_TLV_HEADER_LEN;
    if (total > 0xFFFFu) {
        w->error = CP_WIRE_ERR_LENGTH;
        return;
    }
    w->buf[pos] = (uint8_t)(total >> 8);
    w->buf[pos + 1u] = (uint8_t)total;
}

static void writer_u16_list(cp_writer *w, const uint16_t *values, size_t count)
{
    size_t i;

    if (values == NULL || count == 0u) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    for (i = 0; i < count; i++) {
        writer_be16(w, values[i]);
    }
}

static void writer_frame_end(cp_writer *w)
{
    size_t total;

    if (w->error != CP_WIRE_OK) {
        return;
    }
    if (w->depth != 0u || w->len < CP_WIRE_HEADER_LEN) {
        w->error = CP_WIRE_ERR_ARGUMENT;
        return;
    }
    total = w->len;
    if (total > CP_WIRE_MAX_FRAME || total > 0xFFFFu) {
        w->error = CP_WIRE_ERR_LENGTH;
        return;
    }
    w->buf[2] = (uint8_t)(total >> 8);
    w->buf[3] = (uint8_t)total;
}

static cp_wire_status writer_finish(cp_writer *w, size_t *out_len)
{
    if (w->error != CP_WIRE_OK) {
        *out_len = 0u;
        return w->error;
    }
    *out_len = w->len;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_peek(const uint8_t *buf, size_t len, uint16_t *msg_id,
                            size_t *frame_len)
{
    uint16_t start;
    uint16_t total;

    if (buf == NULL || msg_id == NULL || frame_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (len < CP_WIRE_HEADER_LEN) {
        return CP_WIRE_ERR_TRUNCATED;
    }
    start = be16(buf);
    total = be16(buf + 2u);
    if (start != CP_WIRE_CSM_START) {
        return CP_WIRE_ERR_HEADER;
    }
    if (total < CP_WIRE_HEADER_LEN || total > CP_WIRE_MAX_FRAME) {
        return CP_WIRE_ERR_LENGTH;
    }
    if ((size_t)total > len) {
        return CP_WIRE_ERR_TRUNCATED;
    }
    *msg_id = be16(buf + 4u);
    *frame_len = total;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_open(const uint8_t *buf, size_t len, cp_wire_view *out)
{
    uint16_t msg_id;
    size_t frame_len;
    cp_wire_status st;

    if (out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    st = cp_wire_peek(buf, len, &msg_id, &frame_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    out->frame = buf;
    out->frame_len = frame_len;
    out->msg_id = msg_id;
    out->params = buf + CP_WIRE_HEADER_LEN;
    out->params_len = frame_len - CP_WIRE_HEADER_LEN;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_tlv_iter_init(cp_wire_tlv_iter *it,
                                     const uint8_t *params, size_t params_len)
{
    if (it == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (params == NULL && params_len != 0u) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    it->data = params;
    it->len = params_len;
    it->offset = 0;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_tlv_iter_next(cp_wire_tlv_iter *it, uint16_t *id,
                                     const uint8_t **payload,
                                     size_t *payload_len)
{
    uint16_t tlv_len;
    size_t remaining;

    if (it == NULL || id == NULL || payload == NULL || payload_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (it->offset == it->len) {
        return CP_WIRE_END;
    }
    if (it->offset > it->len) {
        return CP_WIRE_ERR_TLV;
    }
    remaining = it->len - it->offset;
    if (remaining < CP_WIRE_TLV_HEADER_LEN) {
        return CP_WIRE_ERR_TLV;
    }
    tlv_len = be16(it->data + it->offset);
    if (tlv_len < CP_WIRE_TLV_HEADER_LEN) {
        return CP_WIRE_ERR_TLV;
    }
    if ((size_t)tlv_len > remaining) {
        return CP_WIRE_ERR_TRUNCATED;
    }
    *id = be16(it->data + it->offset + 2u);
    *payload = it->data + it->offset + CP_WIRE_TLV_HEADER_LEN;
    *payload_len = (size_t)tlv_len - CP_WIRE_TLV_HEADER_LEN;
    it->offset += tlv_len;
    return CP_WIRE_OK;
}

static cp_wire_status mark_seen(uint32_t *seen, uint16_t id, uint16_t max_id)
{
    uint32_t bit;

    if (seen == NULL || id > max_id || id > CP_TLV_MAX_ID) {
        return CP_WIRE_ERR_PARAM;
    }
    bit = (uint32_t)1u << id;
    if ((*seen & bit) != 0u) {
        return CP_WIRE_ERR_DUPLICATE;
    }
    *seen |= bit;
    return CP_WIRE_OK;
}

/* Rejection flags identify an identification parameter by ID.  Only the
 * fields this checkpoint actually sends are meaningful here; a flag for an
 * unimplemented field is an unknown optional extension and is ignored. */
static bool rejection_param_known(uint16_t id)
{
    switch (id) {
    case 0u:
    case 1u:
    case 2u:
    case 3u:
    case 4u:
    case 5u:
    case 6u:
    case 7u:
    case 8u:
    case 9u:
    case 10u:
    case 11u:
    case 12u:
    case 13u:
    case 17u:
    case 24u:
        return true;
    default:
        return false;
    }
}

static cp_wire_status decode_flag_payload(const uint8_t *payload, size_t len)
{
    (void)payload;
    return (len == 0u) ? CP_WIRE_OK : CP_WIRE_ERR_TYPE;
}

static cp_wire_status decode_optional_bool(const uint8_t *payload, size_t len,
                                           bool *known, bool *value)
{
    if (len == 0u) {
        *known = false;
        *value = false;
        return CP_WIRE_OK;
    }
    if (len != 1u) {
        return CP_WIRE_ERR_TYPE;
    }
    if (payload[0] != 0u && payload[0] != 1u) {
        return CP_WIRE_ERR_TYPE;
    }
    *known = true;
    *value = payload[0] != 0u;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_decode_empty(const cp_wire_view *view,
                                    uint16_t expected_msg_id)
{
    if (view == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != expected_msg_id) {
        return CP_WIRE_ERR_MSG_ID;
    }
    if (view->params_len != 0u) {
        return CP_WIRE_ERR_PARAM;
    }
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_decode_identification_rejection(
    const cp_wire_view *view, cp_wire_identification_rejection *out)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    uint32_t flags = 0;
    bool saw_tlv = false;
    cp_wire_status st;

    if (view == NULL || out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != CP_WIRE_MSG_IDENTIFICATION_REJECTED) {
        return CP_WIRE_ERR_MSG_ID;
    }
    st = cp_wire_tlv_iter_init(&it, view->params, view->params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        saw_tlv = true;
        if (!rejection_param_known(id)) {
            continue;
        }
        st = mark_seen(&seen, id, 24u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        st = decode_flag_payload(payload, payload_len);
        if (st != CP_WIRE_OK) {
            return st;
        }
        flags |= (uint32_t)1u << id;
    }
    if (st != CP_WIRE_END) {
        return st;
    }
    if (!saw_tlv) {
        return CP_WIRE_ERR_MISSING;
    }
    out->flagged = flags;
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_decode_challenge_request(const cp_wire_view *view,
                                                const uint8_t **challenge,
                                                size_t *challenge_len)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    cp_wire_status st;
    bool found = false;

    if (view == NULL || challenge == NULL || challenge_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != CP_WIRE_MSG_AUTH_CHALLENGE_REQUEST) {
        return CP_WIRE_ERR_MSG_ID;
    }
    st = cp_wire_tlv_iter_init(&it, view->params, view->params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        if (id != 0u) {
            /* Structurally validated unknown extension; ignore it. */
            continue;
        }
        st = mark_seen(&seen, id, 0u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        if (payload_len != CP_WIRE_AUTH_CHALLENGE_SHA1 &&
            payload_len != CP_WIRE_AUTH_CHALLENGE_SHA256) {
            /* AA02 carries an MFi digest: SHA-1 or SHA-256 only. */
            return CP_WIRE_ERR_RANGE;
        }
        *challenge = payload;
        *challenge_len = payload_len;
        found = true;
    }
    if (st != CP_WIRE_END) {
        return st;
    }
    return found ? CP_WIRE_OK : CP_WIRE_ERR_MISSING;
}

static cp_wire_status decode_availability_side(const uint8_t *params,
                                               size_t params_len,
                                               cp_wire_availability_side *out)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    cp_wire_status st;

    if (out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    st = cp_wire_tlv_iter_init(&it, params, params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        if (id > 1u) {
            /* Structurally validated unknown extension; ignore it. */
            continue;
        }
        st = mark_seen(&seen, id, 1u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        if (id == 0u) {
            st = decode_optional_bool(payload, payload_len,
                                      &out->available_known, &out->available);
            if (st != CP_WIRE_OK) {
                return st;
            }
        } else {
            st = copy_string_payload(payload, payload_len,
                                     CP_WIRE_MAX_TRANSPORT_ID, true,
                                     out->transport_id,
                                     sizeof(out->transport_id));
            if (st != CP_WIRE_OK) {
                return st;
            }
        }
    }
    return (st == CP_WIRE_END) ? CP_WIRE_OK : st;
}

cp_wire_status cp_wire_decode_availability(const cp_wire_view *view,
                                           cp_wire_availability *out)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    cp_wire_status st;

    if (view == NULL || out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != CP_WIRE_MSG_CARPLAY_AVAILABILITY) {
        return CP_WIRE_ERR_MSG_ID;
    }
    memset(out, 0, sizeof(*out));
    st = cp_wire_tlv_iter_init(&it, view->params, view->params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        if (id > 1u) {
            /* Structurally validated unknown extension; ignore it. */
            continue;
        }
        st = mark_seen(&seen, id, 1u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        if (id == 0u) {
            out->wired.present = true;
            st = decode_availability_side(payload, payload_len, &out->wired);
        } else {
            out->wireless.present = true;
            st = decode_availability_side(payload, payload_len,
                                          &out->wireless);
        }
        if (st != CP_WIRE_OK) {
            return st;
        }
    }
    if (st != CP_WIRE_END) {
        return st;
    }
    if (!out->wired.present && !out->wireless.present) {
        return CP_WIRE_ERR_MISSING;
    }
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_decode_wireless_update(const cp_wire_view *view,
                                              cp_wire_wireless_update *out)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    cp_wire_status st;
    bool found = false;

    if (view == NULL || out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != CP_WIRE_MSG_WIRELESS_UPDATE) {
        return CP_WIRE_ERR_MSG_ID;
    }
    st = cp_wire_tlv_iter_init(&it, view->params, view->params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        if (id != 0u) {
            /* Structurally validated unknown extension; ignore it. */
            continue;
        }
        st = mark_seen(&seen, id, 0u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        if (payload_len != 1u ||
            (payload[0] != 0u && payload[0] != 1u)) {
            return CP_WIRE_ERR_TYPE;
        }
        out->available = payload[0] != 0u;
        found = true;
    }
    if (st != CP_WIRE_END) {
        return st;
    }
    return found ? CP_WIRE_OK : CP_WIRE_ERR_MISSING;
}

cp_wire_status cp_wire_decode_transport_ids(const cp_wire_view *view,
                                            cp_wire_transport_ids *out)
{
    cp_wire_tlv_iter it;
    uint16_t id;
    const uint8_t *payload;
    size_t payload_len;
    uint32_t seen = 0;
    cp_wire_status st;

    if (view == NULL || out == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (view->msg_id != CP_WIRE_MSG_TRANSPORT_IDS) {
        return CP_WIRE_ERR_MSG_ID;
    }
    memset(out, 0, sizeof(*out));
    st = cp_wire_tlv_iter_init(&it, view->params, view->params_len);
    if (st != CP_WIRE_OK) {
        return st;
    }
    while ((st = cp_wire_tlv_iter_next(&it, &id, &payload, &payload_len)) ==
           CP_WIRE_OK) {
        if (id > 1u) {
            /* Structurally validated unknown extension; ignore it. */
            continue;
        }
        st = mark_seen(&seen, id, 1u);
        if (st != CP_WIRE_OK) {
            return st;
        }
        if (id == 0u) {
            st = copy_string_payload(payload, payload_len,
                                     CP_WIRE_MAX_TRANSPORT_ID, true,
                                     out->bluetooth_transport_id,
                                     sizeof(out->bluetooth_transport_id));
        } else {
            st = copy_string_payload(payload, payload_len,
                                     CP_WIRE_MAX_TRANSPORT_ID, true,
                                     out->usb_transport_id,
                                     sizeof(out->usb_transport_id));
        }
        if (st != CP_WIRE_OK) {
            return st;
        }
    }
    if (st != CP_WIRE_END) {
        return st;
    }
    if ((seen & 0x1u) == 0u || (seen & 0x2u) == 0u) {
        return CP_WIRE_ERR_MISSING;
    }
    return CP_WIRE_OK;
}

static cp_wire_status check_common_strings(const cp_wire_identification *id)
{
    if (id->name == NULL || id->model_identifier == NULL ||
        id->manufacturer == NULL || id->serial_number == NULL ||
        id->firmware_version == NULL || id->hardware_version == NULL ||
        id->eap_name == NULL || id->current_language == NULL ||
        id->supported_languages == NULL ||
        id->bluetooth_transport_name == NULL ||
        id->wireless_transport_name == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (id->supported_language_count == 0u ||
        id->supported_language_count > CP_WIRE_MAX_LANGUAGES) {
        return CP_WIRE_ERR_RANGE;
    }
    if (!cp_wire_bt_mac_usable(id->bluetooth_mac)) {
        return CP_WIRE_ERR_RANGE;
    }
    return CP_WIRE_OK;
}

static cp_wire_status encode_auth_blob(uint16_t msg_id, const uint8_t *data,
                                       size_t len, uint8_t *out,
                                       size_t out_cap, size_t *out_len)
{
    cp_writer w;

    writer_init(&w, out, out_cap);
    writer_frame_begin(&w, msg_id);
    writer_tlv_begin(&w, 0u);
    writer_bytes(&w, data, len);
    writer_tlv_end(&w);
    writer_frame_end(&w);
    return writer_finish(&w, out_len);
}

cp_wire_status cp_wire_encode_identification(const cp_wire_identification *id,
                                             uint8_t *out, size_t out_cap,
                                             size_t *out_len)
{
    cp_writer w;
    size_t i;
    cp_wire_status st;

    if (id == NULL || out == NULL || out_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    *out_len = 0u;
    st = check_common_strings(id);
    if (st != CP_WIRE_OK) {
        return st;
    }
    for (i = 0; i < id->supported_language_count; i++) {
        if (id->supported_languages[i] == NULL) {
            return CP_WIRE_ERR_ARGUMENT;
        }
        if (bounded_strlen(id->supported_languages[i], CP_WIRE_MAX_LANGUAGE) >
                CP_WIRE_MAX_LANGUAGE ||
            bounded_strlen(id->supported_languages[i],
                           CP_WIRE_MAX_LANGUAGE) == 0u) {
            return CP_WIRE_ERR_RANGE;
        }
        if (!cp_wire_utf8_valid(
                (const uint8_t *)id->supported_languages[i],
                bounded_strlen(id->supported_languages[i],
                               CP_WIRE_MAX_LANGUAGE))) {
            return CP_WIRE_ERR_UTF8;
        }
    }
    if (id->app_match_team_id != NULL &&
        id->app_match_team_id[0] != '\0' &&
        bounded_strlen(id->app_match_team_id, CP_WIRE_MAX_TEAM_ID) >
            CP_WIRE_MAX_TEAM_ID) {
        return CP_WIRE_ERR_RANGE;
    }

    writer_init(&w, out, out_cap);
    writer_frame_begin(&w, CP_WIRE_MSG_IDENTIFICATION_INFO);

    writer_tlv_begin(&w, 0u);
    writer_text(&w, id->name, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, id->model_identifier, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_text(&w, id->manufacturer, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 3u);
    writer_text(&w, id->serial_number, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 4u);
    writer_text(&w, id->firmware_version, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 5u);
    writer_text(&w, id->hardware_version, 1u, CP_WIRE_MAX_IDENTITY);
    writer_tlv_end(&w);

    writer_tlv_begin(&w, 6u);
    writer_u16_list(&w, k_accessory_messages,
                    sizeof(k_accessory_messages) /
                        sizeof(k_accessory_messages[0]));
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 7u);
    writer_u16_list(&w, k_phone_messages,
                    sizeof(k_phone_messages) / sizeof(k_phone_messages[0]));
    writer_tlv_end(&w);

    writer_tlv_begin(&w, 8u);
    writer_u8(&w, 0u); /* power providing capability: none */
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 9u);
    writer_be16(&w, id->maximum_current_drawn_ma);
    writer_tlv_end(&w);

    writer_tlv_begin(&w, 10u);
    writer_tlv_begin(&w, 0u);
    writer_u8(&w, 1u);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, id->eap_name, 1u, CP_WIRE_MAX_EAP_NAME);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_u8(&w, 4u); /* no action, no communication */
    writer_tlv_end(&w);
    /* EAP component field 4 is the car_play flag.  It is omitted: the flag
     * is not proven true for this accessory, and an empty TLV would encode
     * true.  CarPlay is advertised through transport component 24 below. */
    writer_tlv_end(&w);

    if (id->app_match_team_id != NULL && id->app_match_team_id[0] != '\0') {
        writer_tlv_begin(&w, 11u);
        writer_text(&w, id->app_match_team_id, 1u, CP_WIRE_MAX_TEAM_ID);
        writer_tlv_end(&w);
    }

    writer_tlv_begin(&w, 12u);
    writer_text(&w, id->current_language, 1u, CP_WIRE_MAX_LANGUAGE);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 13u);
    for (i = 0; i < id->supported_language_count; i++) {
        writer_text(&w, id->supported_languages[i], 1u,
                    CP_WIRE_MAX_LANGUAGE);
    }
    writer_tlv_end(&w);

    writer_tlv_begin(&w, 17u);
    writer_tlv_begin(&w, 0u);
    writer_be16(&w, 0u);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, id->bluetooth_transport_name, 1u,
                CP_WIRE_MAX_TRANSPORT_NAME);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 3u);
    writer_bytes(&w, id->bluetooth_mac, 6u);
    writer_tlv_end(&w);
    writer_tlv_end(&w);

    writer_tlv_begin(&w, 24u);
    writer_tlv_begin(&w, 0u);
    writer_be16(&w, 1u);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, id->wireless_transport_name, 1u, CP_WIRE_MAX_SSID);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 4u);
    writer_tlv_end(&w);
    writer_tlv_end(&w);

    writer_frame_end(&w);
    return writer_finish(&w, out_len);
}

cp_wire_status cp_wire_encode_auth_certificate(const uint8_t *certificate,
                                               size_t certificate_len,
                                               uint8_t *out, size_t out_cap,
                                               size_t *out_len)
{
    if (certificate == NULL || out == NULL || out_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (certificate_len == 0u || certificate_len > CP_WIRE_MAX_AUTH_BLOB) {
        return CP_WIRE_ERR_RANGE;
    }
    return encode_auth_blob(CP_WIRE_MSG_AUTH_CERTIFICATE, certificate,
                            certificate_len, out, out_cap, out_len);
}

cp_wire_status cp_wire_encode_auth_response(const uint8_t *response,
                                            size_t response_len,
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len)
{
    if (response == NULL || out == NULL || out_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    if (response_len == 0u || response_len > CP_WIRE_MAX_AUTH_BLOB) {
        return CP_WIRE_ERR_RANGE;
    }
    return encode_auth_blob(CP_WIRE_MSG_AUTH_RESPONSE, response, response_len,
                            out, out_cap, out_len);
}

static cp_wire_status validate_wifi_values(const char *ssid,
                                           const char *passphrase,
                                           uint8_t channel)
{
    size_t n;

    if (ssid == NULL || passphrase == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    n = bounded_strlen(ssid, CP_WIRE_MAX_SSID);
    if (n == 0u || n > CP_WIRE_MAX_SSID ||
        !cp_wire_utf8_valid((const uint8_t *)ssid, n)) {
        return CP_WIRE_ERR_RANGE;
    }
    n = bounded_strlen(passphrase, CP_WIRE_MAX_PASSPHRASE);
    if (n < 8u || n > CP_WIRE_MAX_PASSPHRASE ||
        !cp_wire_utf8_valid((const uint8_t *)passphrase, n)) {
        return CP_WIRE_ERR_RANGE;
    }
    if (channel == 0u || channel > 233u) {
        return CP_WIRE_ERR_RANGE;
    }
    return CP_WIRE_OK;
}

cp_wire_status cp_wire_encode_wifi_config(const cp_wire_wifi_config *cfg,
                                          uint8_t *out, size_t out_cap,
                                          size_t *out_len)
{
    cp_writer w;
    cp_wire_status st;

    if (cfg == NULL || out == NULL || out_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    *out_len = 0u;
    st = validate_wifi_values(cfg->ssid, cfg->passphrase, cfg->channel);
    if (st != CP_WIRE_OK) {
        return st;
    }
    writer_init(&w, out, out_cap);
    writer_frame_begin(&w, CP_WIRE_MSG_WIFI_CONFIG_INFO);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, cfg->ssid, 1u, CP_WIRE_MAX_SSID);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_text(&w, cfg->passphrase, 8u, CP_WIRE_MAX_PASSPHRASE);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 3u);
    writer_u8(&w, 2u); /* WPA2 or WPA3 transition */
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 4u);
    writer_u8(&w, cfg->channel);
    writer_tlv_end(&w);
    writer_frame_end(&w);
    return writer_finish(&w, out_len);
}

cp_wire_status cp_wire_encode_start_session(const cp_wire_start_session *start,
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len)
{
    cp_writer w;
    cp_wire_status st;
    uint8_t security_wire = 0u;

    if (start == NULL || out == NULL || out_len == NULL) {
        return CP_WIRE_ERR_ARGUMENT;
    }
    *out_len = 0u;
    st = validate_wifi_values(start->ssid, start->passphrase, start->channel);
    if (st != CP_WIRE_OK) {
        return st;
    }
    if (!cp_wire_link_local_usable(start->link_local)) {
        return CP_WIRE_ERR_RANGE;
    }
    if (start->port == 0u || start->port > 65535u) {
        return CP_WIRE_ERR_RANGE;
    }
    if (start->device_identifier == NULL ||
        bounded_strlen(start->device_identifier,
                       CP_WIRE_MAX_DEVICE_ID) == 0u ||
        bounded_strlen(start->device_identifier, CP_WIRE_MAX_DEVICE_ID) >
            CP_WIRE_MAX_DEVICE_ID) {
        return CP_WIRE_ERR_RANGE;
    }
    if (start->public_key != NULL &&
        bounded_strlen(start->public_key, CP_WIRE_MAX_PUBLIC_KEY) >
            CP_WIRE_MAX_PUBLIC_KEY) {
        return CP_WIRE_ERR_RANGE;
    }
    if (start->source_version != NULL &&
        bounded_strlen(start->source_version, CP_WIRE_MAX_SOURCE_VERSION) >
            CP_WIRE_MAX_SOURCE_VERSION) {
        return CP_WIRE_ERR_RANGE;
    }
    switch (start->security) {
    case CP_WIRE_WIFI_WPA2:
        security_wire = 0u;
        break;
    case CP_WIRE_WIFI_WPA2_WPA3:
        security_wire = 3u;
        break;
    case CP_WIRE_WIFI_WPA3:
        security_wire = 4u;
        break;
    default:
        return CP_WIRE_ERR_RANGE;
    }

    writer_init(&w, out, out_cap);
    writer_frame_begin(&w, CP_WIRE_MSG_CARPLAY_START);
    writer_tlv_begin(&w, 1u);
    writer_tlv_begin(&w, 0u);
    writer_text(&w, start->ssid, 1u, CP_WIRE_MAX_SSID);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 1u);
    writer_text(&w, start->passphrase, 8u, CP_WIRE_MAX_PASSPHRASE);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_u8(&w, start->channel);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 3u);
    writer_text(&w, start->link_local, 1u, CP_WIRE_MAX_LINK_LOCAL);
    writer_tlv_end(&w);
    if (security_wire != 0u) {
        writer_tlv_begin(&w, 4u);
        writer_u8(&w, security_wire);
        writer_tlv_end(&w);
    }
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 2u);
    writer_be32(&w, start->port);
    writer_tlv_end(&w);
    writer_tlv_begin(&w, 3u);
    writer_text(&w, start->device_identifier, 1u, CP_WIRE_MAX_DEVICE_ID);
    writer_tlv_end(&w);
    if (start->public_key != NULL && start->public_key[0] != '\0') {
        writer_tlv_begin(&w, 4u);
        writer_text(&w, start->public_key, 1u, CP_WIRE_MAX_PUBLIC_KEY);
        writer_tlv_end(&w);
    }
    if (start->source_version != NULL && start->source_version[0] != '\0') {
        writer_tlv_begin(&w, 5u);
        writer_text(&w, start->source_version, 1u,
                    CP_WIRE_MAX_SOURCE_VERSION);
        writer_tlv_end(&w);
    }
    writer_frame_end(&w);
    return writer_finish(&w, out_len);
}

const char *cp_wire_status_name(cp_wire_status status)
{
    switch (status) {
    case CP_WIRE_OK:
        return "ok";
    case CP_WIRE_END:
        return "end";
    case CP_WIRE_ERR_ARGUMENT:
        return "argument";
    case CP_WIRE_ERR_BUFFER:
        return "buffer";
    case CP_WIRE_ERR_HEADER:
        return "header";
    case CP_WIRE_ERR_LENGTH:
        return "length";
    case CP_WIRE_ERR_TRUNCATED:
        return "truncated";
    case CP_WIRE_ERR_TLV:
        return "tlv";
    case CP_WIRE_ERR_MSG_ID:
        return "message-id";
    case CP_WIRE_ERR_DUPLICATE:
        return "duplicate";
    case CP_WIRE_ERR_PARAM:
        return "parameter";
    case CP_WIRE_ERR_TYPE:
        return "type";
    case CP_WIRE_ERR_UTF8:
        return "utf8";
    case CP_WIRE_ERR_MISSING:
        return "missing";
    case CP_WIRE_ERR_UNSUPPORTED:
        return "unsupported";
    case CP_WIRE_ERR_RANGE:
        return "range";
    case CP_WIRE_ERR_DIRECTION:
        return "direction";
    default:
        return "unknown";
    }
}
