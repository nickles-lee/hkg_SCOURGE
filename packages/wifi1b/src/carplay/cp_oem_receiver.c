/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Passive _airplay._tcp observer.  See cp_oem_receiver.h for the trust model.
 */

#define _POSIX_C_SOURCE 200809L

#include "cp_oem_receiver.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CP_OEM_RECEIVER_MAX_PATH 512u
#define CP_OEM_RECEIVER_HASH_CHUNK 16384u
#define CP_OEM_RECEIVER_MAX_FILE_BYTES (64u * 1024u * 1024u)

static void clear_selected(cp_oem_receiver *probe);

/* ------------------------------------------------------------------------- */
/* Small bounded helpers                                                     */
/* ------------------------------------------------------------------------- */

static size_t bounded_strlen(const char *text, size_t max_len)
{
    size_t i;

    if (text == NULL) {
        return 0u;
    }
    for (i = 0u; i < max_len; i++) {
        if (text[i] == '\0') {
            return i;
        }
    }
    return max_len;
}

static bool ascii_key_equal(const uint8_t *key, size_t key_len, const char *literal)
{
    size_t literal_len = strlen(literal);

    return key_len == literal_len && memcmp(key, literal, literal_len) == 0;
}

static bool ascii_case_equal(const char *a, size_t a_len, const char *b)
{
    size_t b_len = strlen(b);
    size_t i;

    if (a_len != b_len) {
        return false;
    }
    for (i = 0u; i < a_len; i++) {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

static bool service_type_matches(const char *regtype, size_t regtype_len)
{
    size_t len = regtype_len;

    if (regtype == NULL) {
        return regtype_len == 0u;
    }
    if (len > 0u && regtype[len - 1u] == '.') {
        len--;
    }
    return ascii_case_equal(regtype, len, CP_OEM_RECEIVER_SERVICE_TYPE);
}

static bool service_domain_matches(const char *domain, size_t domain_len)
{
    size_t len = domain_len;

    if (domain == NULL) {
        return domain_len == 0u;
    }
    if (len == 0u) {
        return true;
    }
    if (domain[len - 1u] == '.') {
        len--;
    }
    return ascii_case_equal(domain, len, "local");
}

static bool is_ascii_hex(uint8_t value)
{
    return (value >= (uint8_t)'0' && value <= (uint8_t)'9') ||
           (value >= (uint8_t)'a' && value <= (uint8_t)'f') ||
           (value >= (uint8_t)'A' && value <= (uint8_t)'F');
}

static bool device_id_valid(const char *value, size_t value_len)
{
    size_t i;

    if (value == NULL || value_len != 17u) {
        return false;
    }
    for (i = 0u; i < value_len; i++) {
        if ((i % 3u) == 2u) {
            if (value[i] != ':') {
                return false;
            }
        } else if (!is_ascii_hex((uint8_t)value[i])) {
            return false;
        }
    }
    return true;
}

static bool source_version_printable(const char *value, size_t value_len)
{
    size_t i;

    if (value == NULL || value_len == 0u || value_len >= CP_OEM_RECEIVER_MAX_SRCVERS) {
        return false;
    }
    for (i = 0u; i < value_len; i++) {
        uint8_t c = (uint8_t)value[i];

        if (c < 0x20u || c > 0x7eu) {
            return false;
        }
    }
    return true;
}

static bool utf8_valid_text(const uint8_t *text, size_t len, bool reject_controls)
{
    size_t i = 0u;

    if (text == NULL && len != 0u) {
        return false;
    }
    while (i < len) {
        uint8_t c = text[i];

        if (c == 0u) {
            return false;
        }
        if (c <= 0x7fu) {
            if (reject_controls && (c < 0x20u || c == 0x7fu)) {
                return false;
            }
            i++;
            continue;
        }
        if (c >= 0xc2u && c <= 0xdfu) {
            if (i + 1u >= len || (text[i + 1u] & 0xc0u) != 0x80u) {
                return false;
            }
            i += 2u;
            continue;
        }
        if (c >= 0xe0u && c <= 0xefu) {
            uint8_t c1;
            uint8_t c2;

            if (i + 2u >= len) {
                return false;
            }
            c1 = text[i + 1u];
            c2 = text[i + 2u];
            if ((c2 & 0xc0u) != 0x80u) {
                return false;
            }
            if (c == 0xe0u) {
                if (c1 < 0xa0u || c1 > 0xbfu) {
                    return false;
                }
            } else if (c == 0xedu) {
                if (c1 < 0x80u || c1 > 0x9fu) {
                    return false;
                }
            } else if (c1 < 0x80u || c1 > 0xbfu) {
                return false;
            }
            i += 3u;
            continue;
        }
        if (c >= 0xf0u && c <= 0xf4u) {
            uint8_t c1;
            uint8_t c2;
            uint8_t c3;

            if (i + 3u >= len) {
                return false;
            }
            c1 = text[i + 1u];
            c2 = text[i + 2u];
            c3 = text[i + 3u];
            if ((c2 & 0xc0u) != 0x80u || (c3 & 0xc0u) != 0x80u) {
                return false;
            }
            if (c == 0xf0u) {
                if (c1 < 0x90u || c1 > 0xbfu) {
                    return false;
                }
            } else if (c == 0xf4u) {
                if (c1 < 0x80u || c1 > 0x8fu) {
                    return false;
                }
            } else if (c1 < 0x80u || c1 > 0xbfu) {
                return false;
            }
            i += 4u;
            continue;
        }
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------------- */
/* SHA-256                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t bit_length;
    uint8_t block[64];
    size_t block_len;
} cp_sha256_ctx;

static uint32_t rotr32(uint32_t value, unsigned shift)
{
    return (value >> shift) | (value << (32u - shift));
}

static void sha256_transform(cp_sha256_ctx *ctx, const uint8_t block[64])
{
    static const uint32_t k[64] = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
        0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
        0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
        0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
        0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
        0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
        0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
        0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
        0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
        0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
        0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
        0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
    };
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0u; i < 16u; i++) {
        w[i] = ((uint32_t)block[i * 4u] << 24) |
               ((uint32_t)block[i * 4u + 1u] << 16) |
               ((uint32_t)block[i * 4u + 2u] << 8) |
               (uint32_t)block[i * 4u + 3u];
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^ rotr32(w[i - 15u], 18u) ^
                      (w[i - 15u] >> 3u);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^ rotr32(w[i - 2u], 19u) ^
                      (w[i - 2u] >> 10u);

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

    for (i = 0u; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
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

static void sha256_init(cp_sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->bit_length = 0u;
    ctx->block_len = 0u;
}

static void sha256_update(cp_sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    size_t offset = 0u;

    if (data == NULL && len != 0u) {
        return;
    }
    ctx->bit_length += (uint64_t)len * 8u;
    while (offset < len) {
        size_t room = 64u - ctx->block_len;
        size_t take = len - offset;

        if (take > room) {
            take = room;
        }
        memcpy(ctx->block + ctx->block_len, data + offset, take);
        ctx->block_len += take;
        offset += take;
        if (ctx->block_len == 64u) {
            sha256_transform(ctx, ctx->block);
            ctx->block_len = 0u;
        }
    }
}

static void sha256_final(cp_sha256_ctx *ctx, uint8_t out[32])
{
    size_t i;

    ctx->block[ctx->block_len++] = 0x80u;
    if (ctx->block_len > 56u) {
        while (ctx->block_len < 64u) {
            ctx->block[ctx->block_len++] = 0u;
        }
        sha256_transform(ctx, ctx->block);
        ctx->block_len = 0u;
    }
    while (ctx->block_len < 56u) {
        ctx->block[ctx->block_len++] = 0u;
    }
    for (i = 0u; i < 8u; i++) {
        ctx->block[56u + i] = (uint8_t)(ctx->bit_length >> (56u - 8u * i));
    }
    sha256_transform(ctx, ctx->block);
    for (i = 0u; i < 8u; i++) {
        out[i * 4u] = (uint8_t)(ctx->state[i] >> 24);
        out[i * 4u + 1u] = (uint8_t)(ctx->state[i] >> 16);
        out[i * 4u + 2u] = (uint8_t)(ctx->state[i] >> 8);
        out[i * 4u + 3u] = (uint8_t)ctx->state[i];
    }
}

cp_oem_receiver_status cp_oem_receiver_sha256(const void *data, size_t len,
                                              uint8_t out[CP_OEM_RECEIVER_SHA256_LEN])
{
    cp_sha256_ctx ctx;

    if (out == NULL || (data == NULL && len != 0u)) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)data, len);
    sha256_final(&ctx, out);
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_sha256_file(
    const char *path, uint8_t out[CP_OEM_RECEIVER_SHA256_LEN])
{
    uint8_t buffer[CP_OEM_RECEIVER_HASH_CHUNK];
    cp_sha256_ctx ctx;
    struct stat st;
    uint64_t total = 0u;
    int fd;

    if (path == NULL || out == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    do {
        fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        return CP_OEM_RECEIVER_ERR_IO;
    }
    if (fstat(fd, &st) != 0) {
        (void)close(fd);
        return CP_OEM_RECEIVER_ERR_IO;
    }
    if (!S_ISREG(st.st_mode) || st.st_size < 0 ||
        (uint64_t)st.st_size > (uint64_t)CP_OEM_RECEIVER_MAX_FILE_BYTES) {
        (void)close(fd);
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }

    sha256_init(&ctx);
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));

        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            (void)close(fd);
            return CP_OEM_RECEIVER_ERR_IO;
        }
        if (got == 0) {
            break;
        }
        if ((uint64_t)got > (uint64_t)CP_OEM_RECEIVER_MAX_FILE_BYTES - total) {
            (void)close(fd);
            return CP_OEM_RECEIVER_ERR_BOUNDS;
        }
        total += (uint64_t)got;
        sha256_update(&ctx, buffer, (size_t)got);
    }
    if (close(fd) != 0) {
        return CP_OEM_RECEIVER_ERR_IO;
    }
    sha256_final(&ctx, out);
    return CP_OEM_RECEIVER_OK;
}

/* ------------------------------------------------------------------------- */
/* TXT record parsing                                                        */
/* ------------------------------------------------------------------------- */

static bool txt_key_seen(const uint8_t *txt, size_t limit, const uint8_t *key,
                         size_t key_len)
{
    size_t offset = 0u;

    while (offset < limit) {
        uint8_t item_len = txt[offset++];
        size_t eq = 0u;

        if (item_len == 0u) {
            continue;
        }
        if (offset + (size_t)item_len > limit) {
            return false;
        }
        while (eq < (size_t)item_len && txt[offset + eq] != (uint8_t)'=') {
            eq++;
        }
        if (eq == key_len && memcmp(txt + offset, key, key_len) == 0) {
            return true;
        }
        offset += (size_t)item_len;
    }
    return false;
}

static cp_oem_receiver_status txt_copy_required(char *dst, size_t dst_cap, size_t *dst_len,
                                                const uint8_t *value, size_t value_len)
{
    if (dst == NULL || dst_len == NULL || value == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (value_len >= dst_cap || value_len == 0u) {
        return CP_OEM_RECEIVER_ERR_TXT_VALUE;
    }
    memcpy(dst, value, value_len);
    dst[value_len] = '\0';
    *dst_len = value_len;
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_parse_txt(const uint8_t *txt, size_t txt_len,
                                                 cp_oem_receiver_txt *out)
{
    size_t offset = 0u;

    if (out == NULL || (txt == NULL && txt_len != 0u)) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (txt_len > CP_OEM_RECEIVER_MAX_TXT) {
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }

    while (offset < txt_len) {
        uint8_t item_len = txt[offset++];
        const uint8_t *item;
        size_t eq = 0u;
        size_t value_len;
        const uint8_t *value;
        size_t i;

        if (item_len == 0u) {
            continue;
        }
        if (offset + (size_t)item_len > txt_len) {
            return CP_OEM_RECEIVER_ERR_TXT_MALFORMED;
        }
        item = txt + offset;
        if (memchr(item, 0, (size_t)item_len) != NULL) {
            return CP_OEM_RECEIVER_ERR_TXT_MALFORMED;
        }
        while (eq < (size_t)item_len && item[eq] != (uint8_t)'=') {
            eq++;
        }
        if (eq == 0u) {
            return CP_OEM_RECEIVER_ERR_TXT_MALFORMED;
        }
        for (i = 0u; i < eq; i++) {
            if (item[i] < 0x21u || item[i] > 0x7eu) {
                return CP_OEM_RECEIVER_ERR_TXT_MALFORMED;
            }
        }
        if (txt_key_seen(txt, offset - 1u, item, eq)) {
            return CP_OEM_RECEIVER_ERR_TXT_DUPLICATE;
        }
        if (eq == (size_t)item_len) {
            value = item + eq;
            value_len = 0u;
        } else {
            value = item + eq + 1u;
            value_len = (size_t)item_len - eq - 1u;
        }
        if (!utf8_valid_text(value, value_len, true)) {
            return CP_OEM_RECEIVER_ERR_TXT_UTF8;
        }
        if (ascii_key_equal(item, eq, "deviceid")) {
            cp_oem_receiver_status status;

            if (out->have_device_id) {
                return CP_OEM_RECEIVER_ERR_TXT_DUPLICATE;
            }
            if (!device_id_valid((const char *)value, value_len)) {
                return CP_OEM_RECEIVER_ERR_TXT_VALUE;
            }
            status = txt_copy_required(out->device_id, sizeof(out->device_id),
                                       &out->device_id_len, value, value_len);
            if (status != CP_OEM_RECEIVER_OK) {
                return status;
            }
            out->have_device_id = true;
        } else if (ascii_key_equal(item, eq, "pi")) {
            cp_oem_receiver_status status;

            if (out->have_pi) {
                return CP_OEM_RECEIVER_ERR_TXT_DUPLICATE;
            }
            status = txt_copy_required(out->pi, sizeof(out->pi), &out->pi_len, value,
                                       value_len);
            if (status != CP_OEM_RECEIVER_OK) {
                return status;
            }
            out->have_pi = true;
        } else if (ascii_key_equal(item, eq, "srcvers")) {
            cp_oem_receiver_status status;

            if (out->have_source_version) {
                return CP_OEM_RECEIVER_ERR_TXT_DUPLICATE;
            }
            if (!source_version_printable((const char *)value, value_len)) {
                return CP_OEM_RECEIVER_ERR_TXT_VALUE;
            }
            status = txt_copy_required(out->source_version, sizeof(out->source_version),
                                       &out->source_version_len, value, value_len);
            if (status != CP_OEM_RECEIVER_OK) {
                return status;
            }
            out->have_source_version = true;
        } else {
            out->unknown_count++;
        }
        out->key_count++;
        if (out->key_count > CP_OEM_RECEIVER_MAX_TXT_KEYS) {
            return CP_OEM_RECEIVER_ERR_BOUNDS;
        }
        offset += (size_t)item_len;
    }

    if (!out->have_device_id || !out->have_pi || !out->have_source_version) {
        return CP_OEM_RECEIVER_ERR_TXT_MISSING;
    }
    return cp_oem_receiver_sha256(out->pi, out->pi_len, out->pi_sha256);
}

cp_oem_receiver_status cp_oem_receiver_validate_srv(uint32_t interface_index,
                                                     uint32_t target_interface_index,
                                                     uint16_t port,
                                                     const char *service_name,
                                                     size_t service_name_len)
{
    if (port == 0u) {
        return CP_OEM_RECEIVER_ERR_SRV_PORT;
    }
    if (target_interface_index != 0u && interface_index != target_interface_index) {
        return CP_OEM_RECEIVER_ERR_SRV_INTERFACE;
    }
    if (interface_index == 0u || service_name == NULL || service_name_len == 0u ||
        service_name_len >= CP_OEM_RECEIVER_MAX_SERVICE_NAME) {
        return CP_OEM_RECEIVER_ERR_SRV_NAME;
    }
    if (memchr(service_name, 0, service_name_len) != NULL) {
        return CP_OEM_RECEIVER_ERR_SRV_NAME;
    }
    if (!utf8_valid_text((const uint8_t *)service_name, service_name_len, true)) {
        return CP_OEM_RECEIVER_ERR_SRV_NAME;
    }
    return CP_OEM_RECEIVER_OK;
}

/* ------------------------------------------------------------------------- */
/* Link-local address and interface scope                                    */
/* ------------------------------------------------------------------------- */

static int hex_value(uint8_t c)
{
    if (c >= (uint8_t)'0' && c <= (uint8_t)'9') {
        return (int)(c - (uint8_t)'0');
    }
    if (c >= (uint8_t)'a' && c <= (uint8_t)'f') {
        return (int)(c - (uint8_t)'a') + 10;
    }
    if (c >= (uint8_t)'A' && c <= (uint8_t)'F') {
        return (int)(c - (uint8_t)'A') + 10;
    }
    return -1;
}

static bool interface_name_valid(const char *name)
{
    size_t len;
    size_t i;

    if (name == NULL) {
        return false;
    }
    len = bounded_strlen(name, CP_OEM_RECEIVER_MAX_INTERFACE);
    if (len == 0u || len >= CP_OEM_RECEIVER_MAX_INTERFACE) {
        return false;
    }
    for (i = 0u; i < len; i++) {
        uint8_t c = (uint8_t)name[i];

        if (c < 0x21u || c > 0x7eu || c == (uint8_t)'%' || c == (uint8_t)'/' ||
            c == (uint8_t)':') {
            return false;
        }
    }
    return true;
}

typedef struct {
    char bare[CP_OEM_RECEIVER_MAX_LINK_LOCAL];
    char zone[CP_OEM_RECEIVER_MAX_INTERFACE];
    bool zone_present;
    bool zone_numeric;
    uint32_t zone_index;
} parsed_link_local;

static bool parse_ipv6_link_local(const char *text, parsed_link_local *out)
{
    const char *percent;
    size_t text_len;
    size_t bare_len;
    uint16_t groups[8];
    uint16_t full[8];
    size_t group_count = 0u;
    int compress = -1;
    size_t i;
    const char *p;

    if (text == NULL || out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    text_len = bounded_strlen(text, CP_OEM_RECEIVER_MAX_LINK_LOCAL);
    if (text_len == 0u || text_len >= CP_OEM_RECEIVER_MAX_LINK_LOCAL) {
        return false;
    }
    percent = strchr(text, '%');
    bare_len = percent == NULL ? text_len : (size_t)(percent - text);
    if (bare_len == 0u || bare_len >= sizeof(out->bare)) {
        return false;
    }
    if (percent != NULL) {
        size_t zone_len = text_len - bare_len - 1u;
        bool numeric = true;

        if (zone_len == 0u || zone_len >= sizeof(out->zone)) {
            return false;
        }
        for (i = 0u; i < zone_len; i++) {
            uint8_t c = (uint8_t)percent[1u + i];

            if (c < 0x21u || c > 0x7eu || c == (uint8_t)'%' || c == (uint8_t)'/') {
                return false;
            }
            if (c < (uint8_t)'0' || c > (uint8_t)'9') {
                numeric = false;
            }
        }
        memcpy(out->zone, percent + 1, zone_len);
        out->zone[zone_len] = '\0';
        out->zone_present = true;
        out->zone_numeric = numeric;
        if (numeric) {
            uint32_t value = 0u;

            for (i = 0u; i < zone_len; i++) {
                uint32_t digit = (uint32_t)(percent[1u + i] - '0');

                if (value > (UINT32_MAX - digit) / 10u) {
                    return false;
                }
                value = value * 10u + digit;
            }
            out->zone_index = value;
        }
    }
    memcpy(out->bare, text, bare_len);
    out->bare[bare_len] = '\0';

    memset(groups, 0, sizeof(groups));
    p = out->bare;
    while (*p != '\0') {
        if (p[0] == ':' && p[1] == ':') {
            if (compress >= 0) {
                return false;
            }
            compress = (int)group_count;
            p += 2;
            if (*p == '\0') {
                break;
            }
            continue;
        }
        if (*p == ':') {
            return false;
        }
        {
            uint32_t value = 0u;
            size_t digits = 0u;

            while (digits < 4u) {
                int digit = hex_value((uint8_t)*p);

                if (digit < 0) {
                    break;
                }
                value = (value << 4u) | (uint32_t)digit;
                p++;
                digits++;
            }
            if (digits == 0u || group_count >= 8u) {
                return false;
            }
            groups[group_count++] = (uint16_t)value;
        }
        if (*p == ':') {
            p++;
            if (*p == '\0') {
                return false;
            }
            if (p[0] == ':') {
                if (compress >= 0) {
                    return false;
                }
                compress = (int)group_count;
                p++;
                if (*p == '\0') {
                    break;
                }
            }
        } else if (*p != '\0') {
            return false;
        }
    }

    if (compress < 0) {
        if (group_count != 8u) {
            return false;
        }
        memcpy(full, groups, sizeof(full));
    } else {
        size_t zeros = 8u - group_count;

        if (zeros == 0u) {
            return false;
        }
        for (i = 0u; i < (size_t)compress; i++) {
            full[i] = groups[i];
        }
        for (i = 0u; i < zeros; i++) {
            full[(size_t)compress + i] = 0u;
        }
        for (i = (size_t)compress; i < group_count; i++) {
            full[i + zeros] = groups[i];
        }
    }

    if ((full[0] & 0xffc0u) != 0xfe80u) {
        return false;
    }
    if ((full[4] | full[5] | full[6] | full[7]) == 0u) {
        return false;
    }
    return true;
}

bool cp_oem_receiver_link_local_usable(const char *text)
{
    parsed_link_local parsed;

    return parse_ipv6_link_local(text, &parsed);
}

cp_oem_receiver_status cp_oem_receiver_scope_init(cp_oem_receiver_scope *out,
                                                   const char *interface_name,
                                                   uint32_t interface_index,
                                                   const char *link_local,
                                                   bool local_address_confirmed)
{
    parsed_link_local parsed;

    if (out == NULL || !interface_name_valid(interface_name)) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->interface_name, interface_name, strlen(interface_name) + 1u);
    out->interface_index = interface_index;
    out->local_address_confirmed = local_address_confirmed;
    if (link_local == NULL || link_local[0] == '\0') {
        return CP_OEM_RECEIVER_OK;
    }
    if (!parse_ipv6_link_local(link_local, &parsed)) {
        return CP_OEM_RECEIVER_ERR_SCOPE;
    }
    memcpy(out->link_local, parsed.bare, strlen(parsed.bare) + 1u);
    out->has_link_local = true;
    out->zone_present = parsed.zone_present;
    if (!parsed.zone_present) {
        out->zone_matches = true;
    } else if (parsed.zone_numeric) {
        out->zone_matches = parsed.zone_index == interface_index;
    } else {
        out->zone_matches = strcmp(parsed.zone, interface_name) == 0;
    }
    if (!out->zone_matches) {
        return CP_OEM_RECEIVER_ERR_SCOPE;
    }
    return CP_OEM_RECEIVER_OK;
}

bool cp_oem_receiver_scope_ready(const cp_oem_receiver_scope *scope)
{
    return scope != NULL && scope->interface_name[0] != '\0' &&
           scope->interface_index != 0u && scope->has_link_local &&
           scope->zone_matches && scope->local_address_confirmed;
}

cp_oem_receiver_status cp_oem_receiver_scope_format(const cp_oem_receiver_scope *scope,
                                                    bool include_zone, char *out,
                                                    size_t out_len)
{
    size_t len;

    if (scope == NULL || out == NULL || out_len == 0u) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (!scope->has_link_local) {
        out[0] = '\0';
        return CP_OEM_RECEIVER_ERR_SCOPE;
    }
    len = strlen(scope->link_local);
    if (len + 1u > out_len) {
        out[0] = '\0';
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }
    memcpy(out, scope->link_local, len + 1u);
    if (!include_zone) {
        return CP_OEM_RECEIVER_OK;
    }
    if (scope->interface_name[0] != '\0') {
        size_t name_len = strlen(scope->interface_name);

        if (len + 1u + name_len + 1u > out_len) {
            out[0] = '\0';
            return CP_OEM_RECEIVER_ERR_BOUNDS;
        }
        out[len++] = '%';
        memcpy(out + len, scope->interface_name, name_len + 1u);
        return CP_OEM_RECEIVER_OK;
    }
    if (len + 12u > out_len) {
        out[0] = '\0';
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }
    {
        int written = snprintf(out + len, out_len - len, "%%%lu",
                               (unsigned long)scope->interface_index);

        if (written < 0 || (size_t)written >= out_len - len) {
            out[0] = '\0';
            return CP_OEM_RECEIVER_ERR_BOUNDS;
        }
    }
    return CP_OEM_RECEIVER_OK;
}

/* ------------------------------------------------------------------------- */
/* Probe state machine                                                       */
/* ------------------------------------------------------------------------- */

static void copy_config_string(char *dst, size_t dst_len, const char *src)
{
    size_t len;

    if (dst_len == 0u) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_len) {
        len = dst_len - 1u;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

void cp_oem_receiver_config_default(cp_oem_receiver_config *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    copy_config_string(out->interface_name, sizeof(out->interface_name),
                       CP_OEM_RECEIVER_DEFAULT_INTERFACE);
    out->browse_timeout_ms = 5000u;
    out->resolve_timeout_ms = 2000u;
    out->settle_ms = 500u;
    out->stale_after_ms = 30000u;
    out->max_services = 2u;
}

static bool config_valid(const cp_oem_receiver_config *config)
{
    if (config == NULL || !interface_name_valid(config->interface_name)) {
        return false;
    }
    if (config->browse_timeout_ms == 0u || config->resolve_timeout_ms == 0u ||
        config->stale_after_ms == 0u) {
        return false;
    }
    if (config->max_services == 0u || config->max_services > CP_OEM_RECEIVER_MAX_SERVICES) {
        return false;
    }
    return true;
}

cp_oem_receiver_status cp_oem_receiver_init(cp_oem_receiver *probe,
                                            const cp_oem_receiver_config *config,
                                            const cp_oem_receiver_dns_ops *ops)
{
    if (probe == NULL || !config_valid(config) || ops == NULL || ops->start == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    memset(probe, 0, sizeof(*probe));
    probe->config = *config;
    probe->ops = *ops;
    probe->selected = -1;
    probe->state = CP_OEM_RECEIVER_STATE_IDLE;
    probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
    probe->readiness = CP_OEM_RECEIVER_READINESS_NO_OBSERVATION;
    probe->blockers = CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION;
    probe->reported_readiness = probe->readiness;
    probe->reported_evidence = probe->evidence;
    probe->reported_blockers = probe->blockers;
    return CP_OEM_RECEIVER_OK;
}

static void dns_event_adapter(void *context,
                             const cp_oem_receiver_dns_event *event);

cp_oem_receiver_status cp_oem_receiver_start(cp_oem_receiver *probe, uint64_t now_ms)
{
    cp_oem_receiver_status status;

    if (probe == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (probe->started) {
        return CP_OEM_RECEIVER_ERR_STATE;
    }
    probe->now_ms = now_ms;
    probe->start_ms = now_ms;
    probe->browse_deadline_ms = now_ms + probe->config.browse_timeout_ms;
    probe->started = true;
    probe->state = CP_OEM_RECEIVER_STATE_WAITING;
    probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
    probe->readiness = CP_OEM_RECEIVER_READINESS_NO_OBSERVATION;
    probe->blockers = CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION;
    probe->event_blockers = 0u;
    probe->dns_error = false;
    probe->stale_reported = false;
    probe->timeout_reported = false;
    probe->stable = false;
    probe->ambiguous = false;
    probe->selected = -1;
    probe->selected_valid = false;
    probe->service_count = 0u;
    memset(probe->services, 0, sizeof(probe->services));
    clear_selected(probe);
    probe->observation_generation++;
    probe->readiness_generation++;
    status = probe->ops.start(probe->ops.context, CP_OEM_RECEIVER_SERVICE_TYPE,
                              CP_OEM_RECEIVER_SERVICE_DOMAIN,
                              probe->config.interface_index, dns_event_adapter, probe);
    if (status != CP_OEM_RECEIVER_OK) {
        if (probe->ops.stop != NULL) {
            probe->ops.stop(probe->ops.context);
        }
        probe->started = false;
        probe->state = CP_OEM_RECEIVER_STATE_ERROR;
        probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
        probe->readiness = CP_OEM_RECEIVER_READINESS_NO_OBSERVATION;
        probe->blockers |= CP_OEM_RECEIVER_BLOCKER_DNS_ERROR;
        probe->readiness_generation++;
        return status;
    }
    return CP_OEM_RECEIVER_OK;
}

void cp_oem_receiver_stop(cp_oem_receiver *probe)
{
    if (probe == NULL) {
        return;
    }
    if (probe->started && probe->ops.stop != NULL) {
        probe->ops.stop(probe->ops.context);
    }
    probe->started = false;
    probe->selected = -1;
    probe->service_count = 0u;
    probe->ambiguous = false;
    probe->stable = false;
    memset(probe->services, 0, sizeof(probe->services));
    clear_selected(probe);
    probe->state = CP_OEM_RECEIVER_STATE_IDLE;
    probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
    probe->readiness = CP_OEM_RECEIVER_READINESS_NO_OBSERVATION;
    probe->blockers = CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION;
    probe->reported_local_trusted = false;
    probe->local_port_matches = false;
    probe->local_identity_matches = false;
}

static cp_oem_receiver_service *service_find(cp_oem_receiver *probe, const char *name,
                                             size_t name_len, uint32_t ifindex)
{
    uint32_t i;

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        cp_oem_receiver_service *slot = &probe->services[i];

        if (slot->used && slot->interface_index == ifindex &&
            slot->service_name_len == name_len &&
            memcmp(slot->service_name, name, name_len) == 0) {
            return slot;
        }
    }
    return NULL;
}

static cp_oem_receiver_service *service_alloc(cp_oem_receiver *probe, const char *name,
                                              size_t name_len, uint32_t ifindex,
                                              uint64_t now_ms)
{
    uint32_t i;

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        cp_oem_receiver_service *slot = &probe->services[i];

        if (!slot->used) {
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            memcpy(slot->service_name, name, name_len);
            slot->service_name[name_len] = '\0';
            slot->service_name_len = name_len;
            slot->interface_index = ifindex;
            slot->added_ms = now_ms;
            slot->resolve_deadline_ms = now_ms + probe->config.resolve_timeout_ms;
            probe->service_count++;
            return slot;
        }
    }
    return NULL;
}

static void service_remove(cp_oem_receiver *probe, cp_oem_receiver_service *slot)
{
    if (slot != NULL && slot->used) {
        memset(slot, 0, sizeof(*slot));
        if (probe->service_count > 0u) {
            probe->service_count--;
        }
        probe->observation_generation++;
        probe->readiness_generation++;
    }
}

static bool slot_same_observation(const cp_oem_receiver_service *slot, uint16_t port,
                                  const cp_oem_receiver_txt *txt)
{
    return slot->resolved && slot->port == port &&
           slot->device_id_len == txt->device_id_len &&
           memcmp(slot->device_id, txt->device_id, txt->device_id_len) == 0 &&
           slot->pi_len == txt->pi_len &&
           memcmp(slot->pi, txt->pi, txt->pi_len) == 0 &&
           slot->source_version_len == txt->source_version_len &&
           memcmp(slot->source_version, txt->source_version,
                  txt->source_version_len) == 0;
}

static bool attestation_matches_slot(const cp_oem_receiver *probe,
                                     const cp_oem_receiver_service *slot)
{
    const cp_oem_receiver_attestation *att = &probe->attestation;

    return att->receiver_observed && att->receiver_pid != 0u &&
           att->listener_port == slot->port &&
           att->identity_fingerprint_present &&
           memcmp(att->identity_fingerprint, slot->pi_sha256,
                  CP_OEM_RECEIVER_SHA256_LEN) == 0;
}

static void clear_selected(cp_oem_receiver *probe)
{
    probe->selected = -1;
    probe->selected_valid = false;
    probe->port = 0u;
    probe->device_id[0] = '\0';
    probe->device_id_len = 0u;
    probe->pi[0] = '\0';
    probe->pi_len = 0u;
    probe->source_version[0] = '\0';
    probe->source_version_len = 0u;
    memset(probe->pi_sha256, 0, sizeof(probe->pi_sha256));
    probe->first_seen_ms = 0u;
    probe->last_seen_ms = 0u;
    probe->local_port_matches = false;
    probe->local_identity_matches = false;
    probe->reported_local_trusted = false;
}

static bool scope_matches_config(const cp_oem_receiver *probe)
{
    return probe->scope.interface_name[0] != '\0' &&
           strcmp(probe->scope.interface_name, probe->config.interface_name) == 0 &&
           (probe->config.interface_index == 0u ||
            probe->scope.interface_index == probe->config.interface_index);
}

static void recompute(cp_oem_receiver *probe)
{
    uint32_t candidates[CP_OEM_RECEIVER_MAX_SERVICES];
    uint32_t candidate_count = 0u;
    uint32_t i;
    uint32_t blockers = probe->event_blockers;
    bool scope_ready = scope_matches_config(probe) &&
                       cp_oem_receiver_scope_ready(&probe->scope);
    bool local_trusted = false;

    if (probe->dns_error) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_DNS_ERROR;
    }
    if (probe->timeout_reported) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_TIMEOUT;
    }
    if (probe->stale_reported) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_STALE;
    }
    if (!probe->scope.has_link_local) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_LINK_LOCAL_UNUSABLE;
    }
    if (!probe->scope.local_address_confirmed) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_LOCAL_ADDRESS_UNCONFIRMED;
    }
    if (!scope_ready) {
        blockers |= CP_OEM_RECEIVER_BLOCKER_INTERFACE_NOT_READY;
    }

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        cp_oem_receiver_service *slot = &probe->services[i];

        if (slot->used && slot->resolved && slot->compatible && !slot->stale) {
            candidates[candidate_count++] = i;
        }
        if (slot->used && !slot->resolved && slot->stale) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_RESOLVE_TIMEOUT;
        }
        if (slot->used && slot->resolved && !slot->compatible) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_UNEXPECTED_SOURCE_VERSION;
        }
    }

    if (probe->dns_error) {
        clear_selected(probe);
        probe->state = CP_OEM_RECEIVER_STATE_ERROR;
        probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
        probe->readiness = CP_OEM_RECEIVER_READINESS_NO_OBSERVATION;
        probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_DNS_ERROR;
        probe->ambiguous = false;
        return;
    }
    if (probe->timeout_reported && candidate_count == 0u) {
        clear_selected(probe);
        probe->state = CP_OEM_RECEIVER_STATE_TIMEOUT;
        probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
        probe->readiness = CP_OEM_RECEIVER_READINESS_TIMEOUT;
        probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_TIMEOUT;
        probe->ambiguous = false;
        return;
    }
    if (probe->stale_reported && candidate_count == 0u) {
        clear_selected(probe);
        probe->state = CP_OEM_RECEIVER_STATE_WAITING;
        probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
        probe->readiness = CP_OEM_RECEIVER_READINESS_STALE;
        probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_STALE;
        probe->ambiguous = false;
        return;
    }
    if (candidate_count > 0u) {
        probe->stale_reported = false;
        blockers &= ~(uint32_t)CP_OEM_RECEIVER_BLOCKER_STALE;
    }
    if (candidate_count == 0u) {
        clear_selected(probe);
        probe->ambiguous = false;
        probe->state = CP_OEM_RECEIVER_STATE_WAITING;
        probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
        probe->readiness = scope_ready ? CP_OEM_RECEIVER_READINESS_NO_OBSERVATION
                                       : CP_OEM_RECEIVER_READINESS_INTERFACE_NOT_READY;
        probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION;
        return;
    }

    if (candidate_count == 1u) {
        probe->selected = (int)candidates[0];
    } else {
        int matched = -1;
        uint32_t match_count = 0u;

        for (i = 0u; i < candidate_count; i++) {
            cp_oem_receiver_service *slot = &probe->services[candidates[i]];

            if (attestation_matches_slot(probe, slot)) {
                matched = (int)candidates[i];
                match_count++;
            }
        }
        if (match_count == 1u) {
            probe->selected = matched;
        } else {
            probe->selected = -1;
            probe->ambiguous = true;
            clear_selected(probe);
            probe->state = CP_OEM_RECEIVER_STATE_OBSERVED;
            probe->evidence = CP_OEM_RECEIVER_EVIDENCE_MDNS_UNVERIFIED;
            probe->readiness = CP_OEM_RECEIVER_READINESS_AMBIGUOUS;
            probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_AMBIGUOUS_SERVICES;
            return;
        }
    }

    probe->ambiguous = false;
    if (probe->selected >= 0) {
        const cp_oem_receiver_service *slot = &probe->services[probe->selected];

        probe->selected_valid = true;
        probe->port = slot->port;
        memcpy(probe->device_id, slot->device_id, slot->device_id_len + 1u);
        probe->device_id_len = slot->device_id_len;
        memcpy(probe->pi, slot->pi, slot->pi_len + 1u);
        probe->pi_len = slot->pi_len;
        memcpy(probe->source_version, slot->source_version,
               slot->source_version_len + 1u);
        probe->source_version_len = slot->source_version_len;
        memcpy(probe->pi_sha256, slot->pi_sha256, sizeof(probe->pi_sha256));
        probe->first_seen_ms = slot->added_ms;
        probe->last_seen_ms = slot->last_update_ms;
        probe->state = CP_OEM_RECEIVER_STATE_OBSERVED;

        probe->local_port_matches = probe->attestation.receiver_observed &&
                                    probe->attestation.receiver_pid != 0u &&
                                    probe->attestation.listener_port == slot->port;
        probe->local_identity_matches =
            probe->attestation.identity_fingerprint_present &&
            memcmp(probe->attestation.identity_fingerprint, slot->pi_sha256,
                   CP_OEM_RECEIVER_SHA256_LEN) == 0;

        if (!probe->attestation.receiver_observed ||
            probe->attestation.receiver_pid == 0u) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_LOCAL_RECEIVER_NOT_OBSERVED;
        } else if (!probe->local_port_matches) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_LOCAL_PORT_MISMATCH;
        }
        if (!probe->attestation.identity_fingerprint_present) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_IDENTITY_ATTESTATION_MISSING;
        } else if (!probe->local_identity_matches) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_LOCAL_IDENTITY_MISMATCH;
        }
        if (!probe->stable) {
            blockers |= CP_OEM_RECEIVER_BLOCKER_NOT_SETTLED;
        }

        local_trusted = scope_ready && probe->stable &&
                        probe->attestation.receiver_observed &&
                        probe->local_port_matches && probe->local_identity_matches;
        probe->evidence = local_trusted ? CP_OEM_RECEIVER_EVIDENCE_LOCAL_ATTESTED
                                        : CP_OEM_RECEIVER_EVIDENCE_MDNS_UNVERIFIED;
        if (local_trusted) {
            probe->readiness = CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED;
        } else {
            probe->readiness = CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED;
        }
        probe->reported_local_trusted = local_trusted;
        probe->blockers = blockers;
        return;
    }

    clear_selected(probe);
    probe->state = CP_OEM_RECEIVER_STATE_WAITING;
    probe->evidence = CP_OEM_RECEIVER_EVIDENCE_NONE;
    probe->readiness = scope_ready ? CP_OEM_RECEIVER_READINESS_NO_OBSERVATION
                                   : CP_OEM_RECEIVER_READINESS_INTERFACE_NOT_READY;
    probe->blockers = blockers | CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION;
}

static void dns_event_adapter(void *context,
                             const cp_oem_receiver_dns_event *event)
{
    cp_oem_receiver *probe = (cp_oem_receiver *)context;

    if (probe == NULL || event == NULL) {
        return;
    }
    (void)cp_oem_receiver_on_dns_event(probe, event,
                                       event->observed_ms != 0u ? event->observed_ms
                                                                : probe->now_ms);
}

cp_oem_receiver_status cp_oem_receiver_on_dns_event(
    cp_oem_receiver *probe, const cp_oem_receiver_dns_event *event, uint64_t now_ms)
{
    uint32_t target;
    cp_oem_receiver_status srv_status;

    if (probe == NULL || event == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (now_ms < probe->now_ms) {
        now_ms = probe->now_ms;
    }
    probe->now_ms = now_ms;
    target = probe->config.interface_index;

    if (!service_type_matches(event->regtype, event->regtype_len) ||
        !service_domain_matches(event->domain, event->domain_len) ||
        event->hosttarget_len > CP_OEM_RECEIVER_MAX_HOSTTARGET) {
        probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_SRV_INVALID;
        probe->readiness_generation++;
        recompute(probe);
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }

    if (event->type == CP_OEM_RECEIVER_DNS_ERROR) {
        probe->dns_error = true;
        probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_DNS_ERROR;
        probe->readiness_generation++;
        recompute(probe);
        return CP_OEM_RECEIVER_ERR_DNS;
    }

    if (event->type == CP_OEM_RECEIVER_DNS_ADD) {
        cp_oem_receiver_service *slot;

        if (event->service_name == NULL || event->service_name_len == 0u ||
            event->service_name_len >= CP_OEM_RECEIVER_MAX_SERVICE_NAME ||
            event->interface_index == 0u ||
            (target != 0u && event->interface_index != target)) {
            probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_SRV_INVALID;
            probe->readiness_generation++;
            recompute(probe);
            return CP_OEM_RECEIVER_ERR_SRV_INTERFACE;
        }
        if (!utf8_valid_text((const uint8_t *)event->service_name,
                             event->service_name_len, true)) {
            probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_SRV_INVALID;
            probe->readiness_generation++;
            recompute(probe);
            return CP_OEM_RECEIVER_ERR_SRV_NAME;
        }
        slot = service_find(probe, event->service_name, event->service_name_len,
                            event->interface_index);
        if (slot == NULL) {
            if (probe->service_count >= probe->config.max_services) {
                probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_TOO_MANY_SERVICES;
                probe->readiness_generation++;
                recompute(probe);
                return CP_OEM_RECEIVER_ERR_BOUNDS;
            }
            slot = service_alloc(probe, event->service_name, event->service_name_len,
                                 event->interface_index, now_ms);
            if (slot == NULL) {
                probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_TOO_MANY_SERVICES;
                probe->readiness_generation++;
                recompute(probe);
                return CP_OEM_RECEIVER_ERR_BOUNDS;
            }
        } else {
            slot->added_ms = now_ms;
            slot->resolve_deadline_ms = now_ms + probe->config.resolve_timeout_ms;
            slot->last_update_ms = now_ms;
            slot->stale = false;
        }
        probe->dns_error = false;
        probe->timeout_reported = false;
        probe->event_blockers &= ~(uint32_t)(CP_OEM_RECEIVER_BLOCKER_DNS_ERROR |
                                   CP_OEM_RECEIVER_BLOCKER_SRV_INVALID);
        probe->state = CP_OEM_RECEIVER_STATE_WAITING;
        probe->readiness_generation++;
        recompute(probe);
        return CP_OEM_RECEIVER_OK;
    }

    if (event->type == CP_OEM_RECEIVER_DNS_REMOVE) {
        cp_oem_receiver_service *slot;

        if (event->service_name == NULL || event->service_name_len == 0u ||
            event->service_name_len >= CP_OEM_RECEIVER_MAX_SERVICE_NAME ||
            event->interface_index == 0u ||
            (target != 0u && event->interface_index != target)) {
            probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_SRV_INVALID;
            probe->readiness_generation++;
            recompute(probe);
            return CP_OEM_RECEIVER_ERR_SRV_NAME;
        }
        slot = service_find(probe, event->service_name, event->service_name_len,
                            event->interface_index);
        if (slot != NULL) {
            service_remove(probe, slot);
            probe->stale_reported = false;
            if (probe->service_count < probe->config.max_services) {
                probe->event_blockers &= ~(uint32_t)CP_OEM_RECEIVER_BLOCKER_TOO_MANY_SERVICES;
            }
            recompute(probe);
        }
        probe->readiness_generation++;
        return CP_OEM_RECEIVER_OK;
    }

    if (event->type != CP_OEM_RECEIVER_DNS_RESOLVE) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }

    srv_status = cp_oem_receiver_validate_srv(event->interface_index, target, event->port,
                                              event->service_name,
                                              event->service_name_len);
    if (srv_status != CP_OEM_RECEIVER_OK) {
        probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_SRV_INVALID;
        probe->readiness_generation++;
        recompute(probe);
        return srv_status;
    }
    if (event->txt_len > CP_OEM_RECEIVER_MAX_TXT) {
        probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_TXT_INVALID;
        probe->readiness_generation++;
        recompute(probe);
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }

    {
        cp_oem_receiver_service *slot = service_find(probe, event->service_name,
                                                     event->service_name_len,
                                                     event->interface_index);
        cp_oem_receiver_txt txt;
        cp_oem_receiver_status txt_status;
        bool changed;

        if (slot == NULL) {
            return CP_OEM_RECEIVER_ERR_ARGUMENT;
        }
        txt_status = cp_oem_receiver_parse_txt(event->txt, event->txt_len, &txt);
        if (txt_status != CP_OEM_RECEIVER_OK) {
            slot->resolved = false;
            slot->compatible = false;
            probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_TXT_INVALID;
            probe->readiness_generation++;
            recompute(probe);
            return txt_status;
        }
        changed = !slot_same_observation(slot, event->port, &txt);
        slot->resolved = true;
        slot->stale = false;
        slot->port = event->port;
        memcpy(slot->device_id, txt.device_id, txt.device_id_len + 1u);
        slot->device_id_len = txt.device_id_len;
        memcpy(slot->pi, txt.pi, txt.pi_len + 1u);
        slot->pi_len = txt.pi_len;
        memcpy(slot->source_version, txt.source_version, txt.source_version_len + 1u);
        slot->source_version_len = txt.source_version_len;
        memcpy(slot->pi_sha256, txt.pi_sha256, sizeof(slot->pi_sha256));
        slot->resolved_ms = now_ms;
        slot->last_update_ms = now_ms;
        slot->compatible =
            strcmp(slot->source_version, CP_OEM_RECEIVER_SOURCE_VERSION) == 0;
        probe->event_blockers &= ~(uint32_t)(CP_OEM_RECEIVER_BLOCKER_TXT_INVALID |
                                   CP_OEM_RECEIVER_BLOCKER_RESOLVE_TIMEOUT |
                                   CP_OEM_RECEIVER_BLOCKER_DNS_ERROR |
                                   CP_OEM_RECEIVER_BLOCKER_SRV_INVALID);
        probe->dns_error = false;
        probe->timeout_reported = false;
        probe->stale_reported = false;
        if (changed) {
            probe->observation_generation++;
            probe->stable = probe->config.settle_ms == 0u;
            probe->settle_deadline_ms = now_ms + probe->config.settle_ms;
        }
        probe->readiness_generation++;
        recompute(probe);
    }
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_set_scope(cp_oem_receiver *probe,
                                                 const cp_oem_receiver_scope *scope,
                                                 uint64_t now_ms)
{
    uint32_t old_ifindex;

    if (probe == NULL || scope == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (!interface_name_valid(scope->interface_name) ||
        strcmp(scope->interface_name, probe->config.interface_name) != 0) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (scope->has_link_local &&
        (!cp_oem_receiver_link_local_usable(scope->link_local) || !scope->zone_matches)) {
        return CP_OEM_RECEIVER_ERR_SCOPE;
    }
    old_ifindex = probe->config.interface_index;
    probe->now_ms = now_ms;
    probe->scope = *scope;
    if (scope->interface_index != 0u) {
        probe->config.interface_index = scope->interface_index;
    }
    if (old_ifindex != probe->config.interface_index) {
        memset(probe->services, 0, sizeof(probe->services));
        probe->service_count = 0u;
        probe->selected = -1;
        probe->stable = false;
        probe->stale_reported = false;
        probe->timeout_reported = false;
        probe->dns_error = false;
        probe->event_blockers = 0u;
        probe->browse_deadline_ms = now_ms + probe->config.browse_timeout_ms;
        probe->observation_generation++;
        probe->readiness_generation++;
        if (probe->started) {
            cp_oem_receiver_status status = CP_OEM_RECEIVER_OK;

            if (probe->ops.rebrowse != NULL) {
                status = probe->ops.rebrowse(probe->ops.context,
                                             probe->config.interface_index);
            } else {
                if (probe->ops.stop != NULL) {
                    probe->ops.stop(probe->ops.context);
                }
                status = probe->ops.start(probe->ops.context,
                                          CP_OEM_RECEIVER_SERVICE_TYPE,
                                          CP_OEM_RECEIVER_SERVICE_DOMAIN,
                                          probe->config.interface_index,
                                          dns_event_adapter, probe);
            }
            if (status != CP_OEM_RECEIVER_OK) {
                probe->dns_error = true;
                probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_DNS_ERROR;
            }
        }
    } else {
        probe->readiness_generation++;
    }
    recompute(probe);
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_set_attestation(
    cp_oem_receiver *probe, const cp_oem_receiver_attestation *attestation, uint64_t now_ms)
{
    if (probe == NULL || attestation == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    probe->now_ms = now_ms;
    probe->attestation = *attestation;
    probe->readiness_generation++;
    recompute(probe);
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_poll(cp_oem_receiver *probe, uint64_t now_ms,
                                            uint32_t *changed)
{
    uint32_t result = 0u;
    uint32_t i;
    bool need_recompute = false;

    if (probe == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (now_ms < probe->now_ms) {
        now_ms = probe->now_ms;
    }
    probe->now_ms = now_ms;
    if (!probe->started) {
        if (changed != NULL) {
            *changed = 0u;
        }
        return CP_OEM_RECEIVER_ERR_STATE;
    }

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        cp_oem_receiver_service *slot = &probe->services[i];

        if (slot->used && !slot->resolved && now_ms >= slot->resolve_deadline_ms &&
            !slot->stale) {
            slot->stale = true;
            probe->event_blockers |= CP_OEM_RECEIVER_BLOCKER_RESOLVE_TIMEOUT;
            service_remove(probe, slot);
            probe->readiness_generation++;
            result |= CP_OEM_RECEIVER_CHANGED_SERVICES |
                      CP_OEM_RECEIVER_CHANGED_READINESS;
            need_recompute = true;
        }
    }

    if (probe->selected >= 0) {
        const cp_oem_receiver_service *slot = &probe->services[probe->selected];

        if (!probe->stable && now_ms >= probe->settle_deadline_ms) {
            probe->stable = true;
            probe->readiness_generation++;
            result |= CP_OEM_RECEIVER_CHANGED_READINESS;
            need_recompute = true;
        }
        if (!probe->stale_reported &&
            now_ms - slot->last_update_ms > probe->config.stale_after_ms) {
            probe->services[probe->selected].stale = true;
            probe->stale_reported = true;
            probe->observation_generation++;
            probe->readiness_generation++;
            result |= CP_OEM_RECEIVER_CHANGED_OBSERVATION |
                      CP_OEM_RECEIVER_CHANGED_READINESS;
            need_recompute = true;
        }
    }

    if (probe->service_count == 0u && probe->selected < 0 &&
        !probe->timeout_reported && now_ms >= probe->browse_deadline_ms) {
        probe->timeout_reported = true;
        probe->state = CP_OEM_RECEIVER_STATE_TIMEOUT;
        probe->readiness = CP_OEM_RECEIVER_READINESS_TIMEOUT;
        probe->blockers |= CP_OEM_RECEIVER_BLOCKER_TIMEOUT;
        probe->observation_generation++;
        probe->readiness_generation++;
        result |= CP_OEM_RECEIVER_CHANGED_OBSERVATION |
                  CP_OEM_RECEIVER_CHANGED_READINESS;
        need_recompute = true;
    }

    if (need_recompute) {
        recompute(probe);
    }

    if (probe->observation_generation != probe->reported_observation_generation) {
        probe->reported_observation_generation = probe->observation_generation;
        result |= CP_OEM_RECEIVER_CHANGED_OBSERVATION;
    }
    if (probe->readiness_generation != probe->reported_readiness_generation) {
        probe->reported_readiness_generation = probe->readiness_generation;
        result |= CP_OEM_RECEIVER_CHANGED_READINESS;
    }

    /* Report a readiness change whenever the public evidence changed. */
    if (probe->readiness != probe->reported_readiness ||
        probe->evidence != probe->reported_evidence ||
        probe->blockers != probe->reported_blockers) {
        probe->reported_readiness = probe->readiness;
        probe->reported_evidence = probe->evidence;
        probe->reported_blockers = probe->blockers;
        result |= CP_OEM_RECEIVER_CHANGED_READINESS;
    }
    if (changed != NULL) {
        *changed = result;
    }
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_get_snapshot(const cp_oem_receiver *probe,
                                                    cp_oem_receiver_snapshot *out)
{
    if (probe == NULL || out == NULL) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->state = probe->state;
    out->evidence = probe->evidence;
    out->readiness = probe->readiness;
    out->projection = CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE;
    out->blockers = probe->blockers;
    out->observation_generation = probe->observation_generation;
    out->readiness_generation = probe->readiness_generation;
    out->stable = probe->stable;
    out->ambiguous = probe->ambiguous;
    out->local_trusted = probe->reported_local_trusted;
    out->service_count = probe->service_count;
    out->interface_index = probe->config.interface_index;
    copy_config_string(out->interface_name, sizeof(out->interface_name),
                       probe->config.interface_name);
    memcpy(out->link_local, probe->scope.link_local, sizeof(out->link_local));
    out->link_local_usable = probe->scope.has_link_local;
    out->local_address_confirmed = probe->scope.local_address_confirmed;
    out->interface_ready = cp_oem_receiver_scope_ready(&probe->scope) &&
                           strcmp(probe->scope.interface_name,
                                  probe->config.interface_name) == 0 &&
                           (probe->config.interface_index == 0u ||
                            probe->scope.interface_index ==
                                probe->config.interface_index);
    out->port = probe->port;
    memcpy(out->device_id, probe->device_id, probe->device_id_len + 1u);
    out->device_id_len = probe->device_id_len;
    memcpy(out->pi, probe->pi, probe->pi_len + 1u);
    out->pi_len = probe->pi_len;
    memcpy(out->source_version, probe->source_version, probe->source_version_len + 1u);
    out->source_version_len = probe->source_version_len;
    memcpy(out->pi_sha256, probe->pi_sha256, sizeof(out->pi_sha256));
    out->first_seen_ms = probe->first_seen_ms;
    out->last_seen_ms = probe->last_seen_ms;
    out->local_port_matches = probe->local_port_matches;
    out->local_identity_matches = probe->local_identity_matches;
    return CP_OEM_RECEIVER_OK;
}

static void hex_prefix(const uint8_t *data, size_t len, char *out, size_t out_len)
{
    static const char hex[] = "0123456789abcdef";
    size_t i;
    size_t use = len < 4u ? len : 4u;

    if (out_len == 0u) {
        return;
    }
    if (use * 2u + 1u > out_len) {
        use = (out_len - 1u) / 2u;
    }
    for (i = 0u; i < use; i++) {
        out[i * 2u] = hex[data[i] >> 4];
        out[i * 2u + 1u] = hex[data[i] & 0x0fu];
    }
    out[use * 2u] = '\0';
}

cp_oem_receiver_status cp_oem_receiver_safe_summary(const cp_oem_receiver_snapshot *snapshot,
                                                    char *buf, size_t buf_len)
{
    char digest[9];
    char dev_digest[9];
    uint8_t dev_hash[CP_OEM_RECEIVER_SHA256_LEN];
    int written;

    if (snapshot == NULL || buf == NULL || buf_len == 0u) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    hex_prefix(snapshot->pi_sha256, sizeof(snapshot->pi_sha256), digest, sizeof(digest));
    if (snapshot->device_id_len > 0u &&
        cp_oem_receiver_sha256(snapshot->device_id, snapshot->device_id_len,
                               dev_hash) == CP_OEM_RECEIVER_OK) {
        hex_prefix(dev_hash, sizeof(dev_hash), dev_digest, sizeof(dev_digest));
    } else {
        dev_digest[0] = '-';
        dev_digest[1] = '\0';
    }
    written = snprintf(buf, buf_len,
                       "state=%s evidence=%s readiness=%s projection=%s interface=%s "
                       "ifindex=%u link_local=%s link_local_usable=%d "
                       "address_confirmed=%d interface_ready=%d port=%u "
                       "deviceid_sha256=%s srcvers=%s pi_len=%lu pi_sha256=%s services=%u "
                       "obs_gen=%lu ready_gen=%lu stable=%d ambiguous=%d local_trusted=%d "
                       "blockers=0x%08lx",
                       cp_oem_receiver_state_string(snapshot->state),
                       cp_oem_receiver_evidence_string(snapshot->evidence),
                       cp_oem_receiver_readiness_string(snapshot->readiness),
                       cp_oem_receiver_projection_string(snapshot->projection),
                       snapshot->interface_name[0] != '\0' ? snapshot->interface_name : "-",
                       (unsigned)snapshot->interface_index,
                       snapshot->link_local[0] != '\0' ? snapshot->link_local : "-",
                       snapshot->link_local_usable ? 1 : 0,
                       snapshot->local_address_confirmed ? 1 : 0,
                       snapshot->interface_ready ? 1 : 0,
                       (unsigned)snapshot->port,
                       dev_digest,
                       snapshot->source_version[0] != '\0' ? snapshot->source_version : "-",
                       (unsigned long)snapshot->pi_len, digest,
                       (unsigned)snapshot->service_count,
                       (unsigned long)snapshot->observation_generation,
                       (unsigned long)snapshot->readiness_generation,
                       snapshot->stable ? 1 : 0, snapshot->ambiguous ? 1 : 0,
                       snapshot->local_trusted ? 1 : 0,
                       (unsigned long)snapshot->blockers);
    if (written < 0 || (size_t)written >= buf_len) {
        buf[buf_len - 1u] = '\0';
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }
    return CP_OEM_RECEIVER_OK;
}

/* ------------------------------------------------------------------------- */
/* Pins and status strings                                                   */
/* ------------------------------------------------------------------------- */

/* Local target pins for this reviewed module.  Future work: move these into
 * the shared firmware lock table once that config is ready; do not edit the
 * shared config from this change. */
const cp_oem_receiver_file_pin cp_oem_receiver_target_pins[] = {
    { "app/bin/AppProjection",
      "8f0096bcc0734d4c9766c15bac54c42f302ed7057182c2573b5ac543913960e3" },
    { "app/lib/libAirPlay.so",
      "ef9e69e3b3388d4a7a1d185827c710af25b8ee99d406c60c664a9609b00aada0" },
    { "app/lib/libCoreUtils.so",
      "9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae" },
    { "app/lib/libCarPlayAppFramework.so",
      "e3603e6b0744cf07724cc5cc79ea01ea3d9207d7eaf37c544c7ce26536ecd7a2" },
    { "app/lib/libCarPlayMediaFramework.so",
      "69690210d1aa4dec33c77b0e80f1d3368f8a70bcb507c1b158b6245dc29eb2f3" }
};

const size_t cp_oem_receiver_target_pin_count =
    sizeof(cp_oem_receiver_target_pins) / sizeof(cp_oem_receiver_target_pins[0]);

const cp_oem_receiver_file_pin cp_oem_receiver_dns_sd_pin = {
    "app/lib/libdns_sd.so",
    "6c6849fb0e3619c170ad3b9679f6c60d9ccbfb88e55e8af4210ddf51dfb52592"
};

static int hex_digit_value(char c)
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

static bool digest_matches(const uint8_t digest[CP_OEM_RECEIVER_SHA256_LEN],
                           const char *hex)
{
    size_t i;

    if (hex == NULL || strlen(hex) != CP_OEM_RECEIVER_SHA256_LEN * 2u) {
        return false;
    }
    for (i = 0u; i < CP_OEM_RECEIVER_SHA256_LEN; i++) {
        int high = hex_digit_value(hex[i * 2u]);
        int low = hex_digit_value(hex[i * 2u + 1u]);

        if (high < 0 || low < 0 || digest[i] != (uint8_t)((high << 4) | low)) {
            return false;
        }
    }
    return true;
}

static cp_oem_receiver_status build_path(const char *root, const char *relative,
                                         char *out, size_t out_len)
{
    size_t root_len;
    size_t relative_len;
    bool need_slash;

    if (root == NULL || relative == NULL || out == NULL || out_len == 0u) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    root_len = strlen(root);
    relative_len = strlen(relative);
    need_slash = root_len > 0u && root[root_len - 1u] != '/';
    if (root_len + (need_slash ? 1u : 0u) + relative_len + 1u > out_len) {
        out[0] = '\0';
        return CP_OEM_RECEIVER_ERR_BOUNDS;
    }
    memcpy(out, root, root_len);
    if (need_slash) {
        out[root_len++] = '/';
    }
    memcpy(out + root_len, relative, relative_len + 1u);
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_verify_files(const char *root,
                                                    const cp_oem_receiver_file_pin *pins,
                                                    size_t pin_count, size_t *failed_index)
{
    size_t i;

    if (root == NULL || pins == NULL || pin_count == 0u) {
        return CP_OEM_RECEIVER_ERR_ARGUMENT;
    }
    if (failed_index != NULL) {
        *failed_index = 0u;
    }
    for (i = 0u; i < pin_count; i++) {
        char path[CP_OEM_RECEIVER_MAX_PATH];
        uint8_t digest[CP_OEM_RECEIVER_SHA256_LEN];
        cp_oem_receiver_status status =
            build_path(root, pins[i].relative_path, path, sizeof(path));

        if (status != CP_OEM_RECEIVER_OK) {
            if (failed_index != NULL) {
                *failed_index = i;
            }
            return status;
        }
        status = cp_oem_receiver_sha256_file(path, digest);
        if (status != CP_OEM_RECEIVER_OK) {
            if (failed_index != NULL) {
                *failed_index = i;
            }
            return status;
        }
        if (!digest_matches(digest, pins[i].sha256_hex)) {
            if (failed_index != NULL) {
                *failed_index = i;
            }
            return CP_OEM_RECEIVER_ERR_PIN_MISMATCH;
        }
    }
    return CP_OEM_RECEIVER_OK;
}

cp_oem_receiver_status cp_oem_receiver_verify_target_root(const char *root,
                                                          size_t *failed_index)
{
    cp_oem_receiver_status status;
    size_t dns_failed = 0u;

    status = cp_oem_receiver_verify_files(root, cp_oem_receiver_target_pins,
                                          cp_oem_receiver_target_pin_count, failed_index);
    if (status != CP_OEM_RECEIVER_OK) {
        return status;
    }
    status = cp_oem_receiver_verify_files(root, &cp_oem_receiver_dns_sd_pin, 1u,
                                          &dns_failed);
    if (status != CP_OEM_RECEIVER_OK) {
        if (failed_index != NULL) {
            *failed_index = cp_oem_receiver_target_pin_count + dns_failed;
        }
        return status;
    }
    return CP_OEM_RECEIVER_OK;
}

const char *cp_oem_receiver_status_string(cp_oem_receiver_status status)
{
    switch (status) {
    case CP_OEM_RECEIVER_OK:
        return "ok";
    case CP_OEM_RECEIVER_ERR_ARGUMENT:
        return "argument";
    case CP_OEM_RECEIVER_ERR_BOUNDS:
        return "bounds";
    case CP_OEM_RECEIVER_ERR_STATE:
        return "state";
    case CP_OEM_RECEIVER_ERR_NO_BACKEND:
        return "no_backend";
    case CP_OEM_RECEIVER_ERR_DNS:
        return "dns";
    case CP_OEM_RECEIVER_ERR_TXT_MALFORMED:
        return "txt_malformed";
    case CP_OEM_RECEIVER_ERR_TXT_DUPLICATE:
        return "txt_duplicate";
    case CP_OEM_RECEIVER_ERR_TXT_MISSING:
        return "txt_missing";
    case CP_OEM_RECEIVER_ERR_TXT_UTF8:
        return "txt_utf8";
    case CP_OEM_RECEIVER_ERR_TXT_VALUE:
        return "txt_value";
    case CP_OEM_RECEIVER_ERR_SRV_PORT:
        return "srv_port";
    case CP_OEM_RECEIVER_ERR_SRV_INTERFACE:
        return "srv_interface";
    case CP_OEM_RECEIVER_ERR_SRV_NAME:
        return "srv_name";
    case CP_OEM_RECEIVER_ERR_SCOPE:
        return "scope";
    case CP_OEM_RECEIVER_ERR_IO:
        return "io";
    case CP_OEM_RECEIVER_ERR_HASH:
        return "hash";
    case CP_OEM_RECEIVER_ERR_PIN_MISMATCH:
        return "pin_mismatch";
    default:
        return "unknown";
    }
}

const char *cp_oem_receiver_state_string(cp_oem_receiver_state state)
{
    switch (state) {
    case CP_OEM_RECEIVER_STATE_IDLE:
        return "idle";
    case CP_OEM_RECEIVER_STATE_WAITING:
        return "waiting";
    case CP_OEM_RECEIVER_STATE_OBSERVED:
        return "observed";
    case CP_OEM_RECEIVER_STATE_REMOVED:
        return "removed";
    case CP_OEM_RECEIVER_STATE_TIMEOUT:
        return "timeout";
    case CP_OEM_RECEIVER_STATE_ERROR:
        return "error";
    default:
        return "unknown";
    }
}

const char *cp_oem_receiver_evidence_string(cp_oem_receiver_evidence evidence)
{
    switch (evidence) {
    case CP_OEM_RECEIVER_EVIDENCE_NONE:
        return "none";
    case CP_OEM_RECEIVER_EVIDENCE_MDNS_UNVERIFIED:
        return "mdns_unverified";
    case CP_OEM_RECEIVER_EVIDENCE_LOCAL_ATTESTED:
        return "local_attested";
    default:
        return "unknown";
    }
}

const char *cp_oem_receiver_readiness_string(cp_oem_receiver_readiness readiness)
{
    switch (readiness) {
    case CP_OEM_RECEIVER_READINESS_NO_OBSERVATION:
        return "no_observation";
    case CP_OEM_RECEIVER_READINESS_INTERFACE_NOT_READY:
        return "interface_not_ready";
    case CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED:
        return "observed_unverified";
    case CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED:
        return "local_matched";
    case CP_OEM_RECEIVER_READINESS_AMBIGUOUS:
        return "ambiguous";
    case CP_OEM_RECEIVER_READINESS_STALE:
        return "stale";
    case CP_OEM_RECEIVER_READINESS_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

const char *cp_oem_receiver_projection_string(cp_oem_receiver_projection projection)
{
    switch (projection) {
    case CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE:
        return "unavailable";
    default:
        return "unknown";
    }
}
