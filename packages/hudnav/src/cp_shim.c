/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CarPlay iAP2 RouteGuidance interceptor — HOST-ONLY PROTOTYPE / MODEL.
 * NOT car-validated. NOT deployable. No firmware, USB, device or network access.
 *
 * Design (mirrors the proven MIB LD_PRELOAD pattern, carplay_protocol.md §7):
 *   [FACT, carplay_protocol.md §1/§2] The iAP2 message family is 0x5200 (HU->phone
 *   Start), 0x5201 (Update), 0x5202 (Maneuver), 0x5203 (Stop), 0x5204 (Lanes).
 *   To make iOS send it we must (1) advertise the route-guidance component in the
 *   outgoing IdentificationInformation 0x1D01 (component 30 = 0x001E) and
 *   (2) register the five ids, then send 0x5200.
 *   [FACT, carplay_extension.md §1.2] On this unit the receiving side is an
 *   inline compare-chain dispatcher at iAP2Service-2.0+0x13c0a..0x14e70 with no
 *   nav case; unknown ids fall to the default at 0x14e70.
 *   [FACT, docs/hudnav/CP_PROTOCOL.md §2-4] The concrete, non-ELF-
 *   patching seam is the DYNAMICALLY LINKED glibc `read` symbol in
 *   iAP2Service-2.0: PLT 0x4fe0, R_X86_64_JUMP_SLOT at GOT 0x299dd0
 *   (read@GLIBC_2.2.5), called at 0xddd9 in Transport::recieverThread() as
 *   read(mReaderFd @ object+0x84, pRcvBuf, 0x101d0) after select@plt @0xdce1;
 *   the fd is opened at 0xd55d from devPath (object+0x60), i.e. /dev/ttyIAP20
 *   (0x73375, high-confidence INFERENCE). No DF_SYMBOLIC/BLSYMBOLIC, so an
 *   LD_PRELOAD `read` wins global lookup and sees bytes BEFORE the dispatcher.
 *   The outbound recipe uses `write` (PLT 0x4840, GOT 0x299a00). Neither hook
 *   requires patching the signed vendor ELF.
 *
 * This file models that seam's *logic* as pure functions a host test can drive:
 *   cp_patch_identify()  - byte diff on the Identify frame
 *   cp_reg_*()           - message-id registration config model
 *   cp_build_start()     - the 0x5200 request
 *   cp_find_session()    - 40 40 frame scan over a raw link buffer
 *   cp_handle_message()  - 0x5201/0x5202 parse -> daemon line protocol
 *
 * The real hook (NOT present here — no new LD_PRELOAD .so is shipped by this file)
 * would be an LD_PRELOAD `read` in iAP2Service-2.0, filtered to the /dev/ttyIAP20
 * fd (hook `open` at 0xd55d, or match count == 0x101d0 at 0xddd9), then:
 *   real read FIRST -> cp_find_session() -> cp_handle_message(bytes, len,
 *   cp_sock_emit, NULL) -> return the ORIGINAL buffer/return value UNMODIFIED.
 * Outbound: a `write` hook would apply cp_patch_identify() and submit
 * cp_build_start() through the STOCK transport (it owns seq/ack/checksum —
 * carplay_protocol.md §3, never hand-build FF5A link packets). Deployment is a
 * data-only LD_PRELOAD line in /lib/systemd/system/carplay_iAP2.service.
 *
 * FAIL-OPEN, non-negotiable: every entry point returns "no change / no output"
 * on malformed or unexpected input; no error path alters or drops the real
 * iAP2 bytes, blocks, allocates, or takes ownership of foreign buffers.
 *
 * PROVISIONAL (introduced here; nothing below is car- or VM-validated):
 *   P1. RESOLVED at the symbol level (offline RE, 2026-11-24): the interception
 *       point is `read@GLIBC_2.2.5` in iAP2Service-2.0 — call site 0xddd9
 *       (mReaderFd @ object+0x84, count 0x101d0, gated by select@plt @0xdce1),
 *       GOT JUMP_SLOT 0x299dd0, fd opened at 0xd55d from /dev/ttyIAP20. No ELF
 *       patch needed. RUNTIME INTERPOSITION NOW VM-PROVEN against the real
 *       iAP2Service-2.0 (LD_DEBUG A/B: read@GLIBC_2.2.5 rebinds from libc.so.6
 *       to a preloaded shim; 128 real read calls logged) — see
 *       docs/hudnav/CP_PROTOCOL.md and scripts cp01-cp03 in
 *       tests/vm/. That proof used the separate VM-only
 *       src/cp_read_shim.c; this file remains a host-only logic model. STILL
 *       PROVISIONAL: a hit at the exact seam call (count 0x101d0 / return 0xddde
 *       — no phone/link in the VM, so none was seen), the precise
 *       string->Transport data-flow proving the device path, the Identify
 *       builder buffer and any message-ID registration exchange (the read hook
 *       itself needs no vendor registration — it sees raw bytes before the
 *       dispatcher). See docs/hudnav/CP_PROTOCOL.md §9.
 *   P2. IdentificationInformation param 30 value encoding is still not given in
 *       carplay_protocol.md §2 (only the id/component 0x001E). The real 260319
 *       binary does not implement param 30 at all (docs/hudnav/CP_PROTOCOL.md
 *       §2.3), so no stock value can be copied. We keep TLV 00 1E with a 1-byte
 *       0x01 "supported" flag as a PROVISIONAL placeholder. Resolve: Apple iAP2
 *       spec (carplayd ident::route_guidance) or a captured working Identify.
 *   P3. The 0x5200 request payload (source_name / source_supports_rg / exit_info
 *       named in §2 but with no TLV ids given) is omitted: header-only frame
 *       `40 40 00 06 52 00`, now confirmed as the exact stock serialiser layout
 *       (cp_iap2_impl_spec.md §4). The real transport body is
 *       `[0x02][session][…]`; the session byte is link-chosen and unknown
 *       offline (cp_build_start_link() takes it as a parameter).
 *   P4. 0x01 RouteGuidanceState numeric enum -> daemon STATE s=0/1/2 is not
 *       enumerated in §4 ("guiding/arrived/loading/rerouting/proceed"). We emit
 *       s=1 only for a valid, opted-in update and never synthesize s=2 (reroute).
 *       Resolve: Apple ACCNav_RGUpdate_RouteGuidanceState enum values.
 *   P5. TLV integer widths (1/2/4 B) are assumed big-endian, as in rgd_tlv.c;
 *       iAP2 unit enum for 0x0B/0x0C is not mapped to cluster units 0..4, so the
 *       DIST line uses unit=0 (meters), which is what TLV 0x0A actually carries.
 *       Resolve: live capture + Apple spec type widths.
 *
 * Reviewed carplay_protocol.md §4/§5 in full before writing code: every TLV id
 * needed for a normalized hn_turn / DIST (0x01, 0x02, 0x03, 0x04, 0x07, 0x0A,
 * 0x0E, 0x0F, 0x14; maneuver 0x01, 0x03, 0x04, 0x05, 0x08, 0x09, 0x0A, 0x0B)
 * is ALREADY parsed by rgd_tlv.c. No new TLV tag numbers were invented here.
 *
 * Build (host): gcc -shared -fPIC -o out/cp_shim.so src/cp_shim.c \
 *                   src/rgd_tlv.c src/convert.c
 */
#define _GNU_SOURCE
#include "cp_shim.h"
#include "rgd_tlv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xff);
}

/* Session header: 40 40, len BE16 @+2 (whole msg, >=6), msgid BE16 @+4. */
static bool session_msgid(const uint8_t *b, size_t len, uint16_t *id, size_t *mlen)
{
    if (!b || len < 6 || b[0] != 0x40 || b[1] != 0x40) return false;
    uint16_t m = rd16(b + 2);
    if (m < 6 || m > len) return false;
    if (id) *id = rd16(b + 4);
    if (mlen) *mlen = m;
    return true;
}

/* ---- message-id registration config model -------------------------------- */

void cp_reg_init(cp_msgid_reg *r)
{
    if (r) memset(r, 0, sizeof *r);
}

bool cp_reg_set(cp_msgid_reg *r, uint16_t id, bool sent, bool recv)
{
    if (!r) return false;
    for (size_t i = 0; i < r->n; i++) {
        if (r->e[i].id == id) { r->e[i].sent = sent; r->e[i].recv = recv; return true; }
    }
    if (r->n >= sizeof r->e / sizeof r->e[0]) return false;  /* full: fail-open */
    r->e[r->n].id = id; r->e[r->n].sent = sent; r->e[r->n].recv = recv;
    r->n++;
    return true;
}

bool cp_reg_has(const cp_msgid_reg *r, uint16_t id, bool sent)
{
    if (!r) return false;
    for (size_t i = 0; i < r->n; i++) {
        if (r->e[i].id == id) return sent ? r->e[i].sent : r->e[i].recv;
    }
    return false;
}

size_t cp_reg_rgd_defaults(cp_msgid_reg *r)
{
    if (!r) return 0;
    cp_reg_init(r);
    size_t n = 0;
    if (cp_reg_set(r, CP_MSG_START_RGD, true,  false)) n++;
    if (cp_reg_set(r, CP_MSG_STOP_RGD,   true,  false)) n++;
    if (cp_reg_set(r, CP_MSG_UPDATE,     false, true))  n++;
    if (cp_reg_set(r, CP_MSG_MANEUVER,   false, true))  n++;
    if (cp_reg_set(r, CP_MSG_LANES,      false, true))  n++;
    return n;
}

static cp_msgid_reg cp_active_reg;

size_t cp_shim_register_ids(void) { return cp_reg_rgd_defaults(&cp_active_reg); }
bool cp_shim_id_registered(uint16_t id, bool sent)
{
    return cp_reg_has(&cp_active_reg, id, sent);
}

/* ---- Identify patch ------------------------------------------------------ */

/* True when a raw 2-byte-BE id list already contains `id`. */
static bool id_list_has(const uint8_t *v, size_t vn, uint16_t id)
{
    if (vn & 1) return false;
    for (size_t i = 0; i < vn; i += 2)
        if (rd16(v + i) == id) return true;
    return false;
}

static const uint16_t cp_sent_ids[] = { CP_MSG_START_RGD, CP_MSG_STOP_RGD };
static const uint16_t cp_recv_ids[] = { CP_MSG_UPDATE, CP_MSG_MANEUVER, CP_MSG_LANES };

/* Patch an outgoing IdentificationInformation message (docs/hudnav/CP_PROTOCOL.md
 * §2.4/§3.3). Registration lives in the Identify TLVs, not in a config file:
 *   TLV id 6 (sent)  -> append 52 00, 52 03 as 2-byte big-endian ids
 *   TLV id 7 (recv)  -> append 52 01, 52 02, 52 04
 *   TLV id 30        -> RouteGuidanceDisplayComponent (value still PROVISIONAL)
 * Handles both a bare session message and the real link-body form
 * `[0x02][session][40 40 …]`. Idempotent; fail-open on any malformed input,
 * fixed 16-bit overflow, or insufficient capacity. */
size_t cp_patch_identify(uint8_t *buf, size_t cap, size_t len)
{
    if (!buf || cap < len) return len;                 /* fail-open */

    size_t base = 0;
    if (len >= 8 && buf[0] == 0x02 && buf[2] == 0x40 && buf[3] == 0x40)
        base = 2;                                      /* link body: start at the session msg */

    uint16_t id;
    size_t mlen;
    if (!session_msgid(buf + base, len - base, &id, &mlen) || id != CP_MSG_IDENTIFY)
        return len;
    if (base + mlen != len) return len;                /* trailing bytes: don't guess */

    /* Pre-scan: validate the TLV chain and measure what is missing. */
    bool has_comp = false;
    size_t add6 = 0, add7 = 0;
    size_t off = base + 6;
    while (off + 4 <= len) {
        uint16_t tl = rd16(buf + off), tid = rd16(buf + off + 2);
        if (tl < 4 || tl > len - off) return len;      /* malformed: fail-open */
        const uint8_t *v = buf + off + 4;
        size_t vn = (size_t)tl - 4;
        if (tid == CP_PARAM_RG_COMP) has_comp = true;
        if (tid == 6) {
            if (vn & 1) return len;                    /* odd id list: fail-open */
            for (size_t i = 0; i < sizeof cp_sent_ids / sizeof cp_sent_ids[0]; i++)
                if (!id_list_has(v, vn, cp_sent_ids[i])) add6 += 2;
        }
        if (tid == 7) {
            if (vn & 1) return len;
            for (size_t i = 0; i < sizeof cp_recv_ids / sizeof cp_recv_ids[0]; i++)
                if (!id_list_has(v, vn, cp_recv_ids[i])) add7 += 2;
        }
        off += tl;
    }
    if (off != len) return len;                        /* partial TLV tail: fail-open */
    if (has_comp && add6 == 0 && add7 == 0) return len; /* already patched */

    size_t added = add6 + add7 + (has_comp ? 0 : 5);
    size_t outlen = len + added;
    if (outlen > 0xffff || cap < outlen) return len;   /* no room: fail-open */

    uint8_t *out = malloc(outlen);
    if (!out) return len;

    /* Rebuild: header, then each TLV with any missing ids appended in place. */
    memcpy(out, buf, base + 6);
    size_t o = base + 6;
    off = base + 6;
    while (off + 4 <= len) {
        uint16_t tl = rd16(buf + off), tid = rd16(buf + off + 2);
        const uint8_t *v = buf + off + 4;
        size_t vn = (size_t)tl - 4;
        size_t extra = 0;
        if (tid == 6) extra = add6;
        if (tid == 7) extra = add7;
        wr16(out + o, (uint16_t)(tl + extra));
        wr16(out + o + 2, tid);
        memcpy(out + o + 4, v, vn);
        size_t w = o + 4 + vn;
        if (tid == 6) {
            for (size_t i = 0; i < sizeof cp_sent_ids / sizeof cp_sent_ids[0]; i++)
                if (!id_list_has(v, vn, cp_sent_ids[i])) { wr16(out + w, cp_sent_ids[i]); w += 2; }
        } else if (tid == 7) {
            for (size_t i = 0; i < sizeof cp_recv_ids / sizeof cp_recv_ids[0]; i++)
                if (!id_list_has(v, vn, cp_recv_ids[i])) { wr16(out + w, cp_recv_ids[i]); w += 2; }
        }
        o = w;
        off += tl;
    }
    if (!has_comp) {
        /* TLV [len=5][id=0x001E][value=0x01]. Value encoding = P2 (PROVISIONAL). */
        wr16(out + o, 5); wr16(out + o + 2, CP_PARAM_RG_COMP);
        out[o + 4] = 0x01; o += 5;
    }
    wr16(out + base + 2, (uint16_t)(o - base));        /* whole-message length */
    memcpy(buf, out, o);
    free(out);
    return o;
}

/* ---- 0x5200 request ------------------------------------------------------ */

size_t cp_build_start(uint8_t *buf, size_t cap)
{
    if (!buf || cap < 6) return 0;                     /* fail-open */
    buf[0] = 0x40; buf[1] = 0x40;
    wr16(buf + 2, 6);
    wr16(buf + 4, CP_MSG_START_RGD);
    /* P3: request-parameter TLVs omitted — ids not documented in §2. */
    return 6;
}

size_t cp_build_start_link(uint8_t *buf, size_t cap, uint8_t session)
{
    if (!buf || cap < 8) return 0;
    buf[0] = 0x02; buf[1] = session;                   /* transport link body */
    size_t n = cp_build_start(buf + 2, cap - 2);
    return n ? n + 2 : 0;
}

/* ---- raw-link frame scan (host model of the byte-stream hook) ------------ */

int cp_find_session(const uint8_t *buf, size_t len, size_t *msg_len)
{
    if (!buf || len < 6) return -1;
    for (size_t i = 0; i + 6 <= len; i++) {
        if (buf[i] != 0x40 || buf[i + 1] != 0x40) continue;
        uint16_t m = rd16(buf + i + 2);
        if (m >= 6 && m <= len - i) {
            if (msg_len) *msg_len = m;
            return (int)i;
        }
    }
    return -1;
}

/* ---- 0x5201 / 0x5202 -> line protocol ------------------------------------ */

int cp_handle_message(const uint8_t *msg, size_t len, cp_emit_fn emit, void *ctx)
{
    if (!emit || !msg) return 0;
    uint16_t id; size_t mlen;
    if (!session_msgid(msg, len, &id, &mlen)) return 0;

    char line[128];
    int n = 0;

    if (id == CP_MSG_UPDATE) {
        hn_rgd_state s;
        if (!hn_rgd_parse_update(msg, mlen, &s)) return 0;
        /* §6 gate: only nav apps that opt in (TLV 0x14=1, Apple Maps) send
         * metadata. Fail-closed for OUR forwarded copy when the gate is absent
         * or 0; the stock iAP2 bytes are still passed through by the caller. */
        if (!(s.present & (1u << 0x14)) || s.source_supports_rg != 1) return 0;
        /* P4: state enum is only modeled as "active"; no 0x01->s=2 mapping. */
        snprintf(line, sizeof line, "STATE s=1\n");                 emit(ctx, line); n++;
        /* TLV 0x0A is exact metres (§4); display=exact, unit=0 (metres, P5).
         * meters=-1 (TLV absent) is harmless: motion.c ignores d->meters<0. */
        snprintf(line, sizeof line, "DIST m=%d disp=%d unit=0\n",
                 s.dist_maneuver_m, s.dist_maneuver_m);             emit(ctx, line); n++;
        /* TLV 0x07 = DistanceRemaining (destination), metres. Only forwarded when
         * present; the 0x4E8 cluster frame needs it and has no other source. */
        if (s.dist_dest_m >= 0) {
            snprintf(line, sizeof line, "DEST m=%d\n", s.dist_dest_m);
            emit(ctx, line); n++;
        }
        snprintf(line, sizeof line, "HB\n");                        emit(ctx, line); n++;
        return n;
    }

    if (id == CP_MSG_MANEUVER) {
        hn_rgd_maneuver mv;
        if (!hn_rgd_parse_maneuver(msg, mlen, &mv)) return 0;
        /* §5: EManeuverType is 0..53; 255 = NOT_SET. Reject outside 0..53. */
        if (mv.type < 0 || mv.type > 53) return 0;
        hn_turn t = hn_rgd_to_turn(&mv);
        if (t.man == HN_MAN_UNKNOWN) return 0;
        snprintf(line, sizeof line, "TURN src=cp man=%d exit=%d raw=%d side=%d\n",
                 (int)t.man, t.exit_angle, t.raw_type, t.raw_side);
        emit(ctx, line); n++;
        snprintf(line, sizeof line, "HB\n");              emit(ctx, line); n++;
        return n;
    }

    /* 0x5200/0x5203/0x5204 and unknown ids: no normalized record. The hook
     * still hands the original bytes to the stock stack (fail-open). */
    return 0;
}

/* ---- socket emitter (same unix-socket line protocol as aa_shim.c) -------- */

static int sock = -1;

void cp_sock_close(void)
{
    if (sock >= 0) { close(sock); sock = -1; }
}

static void ensure_sock(void)
{
    if (sock >= 0) return;
    const char *path = getenv("HUDNAV_SOCK");
    if (!path) path = "/tmp/hudnav.sock";
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(sock, (struct sockaddr *)&a, sizeof a) < 0) cp_sock_close();
}

void cp_sock_emit(void *ctx, const char *line)
{
    (void)ctx;
    if (!line) return;
    ensure_sock();
    if (sock < 0) return;
    size_t n = strlen(line);
    if (send(sock, line, n, MSG_NOSIGNAL) < 0) cp_sock_close();
}
