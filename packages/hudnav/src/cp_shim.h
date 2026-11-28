/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CarPlay iAP2 RouteGuidance interceptor PROTOTYPE (host-only model).
 * Public surface for the host unit tests; see cp_shim.c for the design and
 * CONFIRMED / PROVISIONAL labels. NOT car-validated, NOT deployable.
 */
#ifndef HUDNAV_CP_SHIM_H
#define HUDNAV_CP_SHIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* iAP2 message ids used here — ALL of these come from carplay_protocol.md
 * §1/§2/§3. Do not add ids/TLV tags that are not in that document or in
 * carplay_extension.md without a PROVISIONAL comment. */
#define CP_MSG_START_RGD  0x5200u  /* HU -> phone: StartRouteGuidanceUpdates */
#define CP_MSG_UPDATE     0x5201u  /* phone -> HU: RouteGuidanceUpdate */
#define CP_MSG_MANEUVER   0x5202u  /* phone -> HU: RouteGuidanceManeuverUpdate */
#define CP_MSG_STOP_RGD   0x5203u  /* HU -> phone: StopRouteGuidanceUpdates */
#define CP_MSG_LANES      0x5204u  /* phone -> HU: RouteGuidanceLaneGuidanceInformation */
#define CP_MSG_IDENTIFY   0x1D01u  /* IdentificationInformation (IdentificationInformation message) */
#define CP_PARAM_RG_COMP  0x001Eu  /* component 30 = RouteGuidanceDisplayComponent */

/* One emitter call = one newline-terminated line, identical wire format to
 * aa_shim.c / README.md "Line protocol". */
typedef void (*cp_emit_fn)(void *ctx, const char *line);

/* ---- message-id registration model (MIB dio_manager.json analogue) ---- */
typedef struct { uint16_t id; bool sent; bool recv; } cp_msgid_entry;
typedef struct { cp_msgid_entry e[12]; size_t n; } cp_msgid_reg;

void   cp_reg_init(cp_msgid_reg *r);
bool   cp_reg_set(cp_msgid_reg *r, uint16_t id, bool sent, bool recv);
bool   cp_reg_has(const cp_msgid_reg *r, uint16_t id, bool sent);
size_t cp_reg_rgd_defaults(cp_msgid_reg *r);   /* the five documented ids */

/* Active (model) registry used by the shim skeleton. */
size_t cp_shim_register_ids(void);
bool   cp_shim_id_registered(uint16_t id, bool sent);

/* ---- byte-level operations on iAP2 session messages ---- */

/* Patch an outgoing IdentificationInformation (0x1D01) buffer to advertise the
 * route-guidance component and register the 0x52xx message ids. Accepts either a
 * bare session message or the real link-body form `[0x02][session][40 40 …]`.
 * Returns the (possibly unchanged) used length. Fail-open: malformed frames,
 * already-patched frames and insufficient capacity all return `len` unchanged. */
size_t cp_patch_identify(uint8_t *buf, size_t cap, size_t len);

/* Build a header-only 0x5200 StartRouteGuidanceUpdates session message.
 * Returns 6, or 0 if cap < 6. See cp_shim.c for the PROVISIONAL request payload. */
size_t cp_build_start(uint8_t *buf, size_t cap);

/* Wrap cp_build_start() in the transport link body `[0x02][session][…]` that the
 * real write() seam expects. The session byte is link-chosen and must be copied
 * from an observed outbound frame on the car. Returns total length or 0. */
size_t cp_build_start_link(uint8_t *buf, size_t cap, uint8_t session);

/* Locate a 40 40 session message in a raw iAP2 link buffer (host-model scan).
 * Returns byte offset or -1; *msg_len is the in-frame length when found. */
int cp_find_session(const uint8_t *buf, size_t len, size_t *msg_len);

/* Parse 0x5201/0x5202 and emit normalized line-protocol records.
 * Returns the number of lines emitted (0 on any error / unsupported message).
 * The real recv hook must ALWAYS forward the original bytes regardless. */
int cp_handle_message(const uint8_t *msg, size_t len, cp_emit_fn emit, void *ctx);

/* Socket emitter for the real hook: same daemon socket, same line protocol. */
void cp_sock_emit(void *ctx, const char *line);
void cp_sock_close(void);

#endif /* HUDNAV_CP_SHIM_H */
