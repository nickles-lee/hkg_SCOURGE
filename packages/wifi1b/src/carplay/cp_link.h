/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_link — bounded, transport-agnostic iAP2 link engine.
 *
 * This module is deliberately only the iAP2 link layer: detection marker,
 * 0xff5a framing, checksums, SYN/LSP negotiation, ACK/EAK/RST, sequence
 * numbering, retransmission and bounded reassembly. It does not open
 * sockets, use clocks, touch hardware, allocate memory, interpret Control
 * Session Messages (CSM), or own authentication state.
 *
 * The integration caller provides:
 *   - a monotonic millisecond clock (`now_ms`) for every timed call;
 *   - received transport bytes through cp_link_feed();
 *   - periodic service through cp_link_tick() using cp_link_next_deadline();
 *   - drained wire bytes through cp_link_output_peek()/cp_link_output_consume().
 *
 * Wire facts pinned to the LIVI reference tree
 * 64a05b68358db52ed887169143afd5944fd6f1f7
 * native/crates/iap2-link (protocol facts only; no implementation copied):
 *
 *   detect marker      FF 55 02 00 EE 10
 *   header             0xFF 0x5A | length:u16be | control | seq | ack |
 *                      session_id | header_checksum
 *   payload            payload bytes | payload_checksum
 *   length             total wire bytes including the 9-byte header and the
 *                      payload checksum; 9 means no payload
 *   checksums          two's complement such that every byte sum is zero
 *   control            SYN 0x80, ACK 0x40, EAK 0x20, RST 0x10
 *   LSP                0x01 | max_outgoing:u8 | max_len:u16be |
 *                      retransmission_timeout_ms:u16be |
 *                      cumulative_ack_timeout_ms:u16be |
 *                      max_retransmissions:u8 | max_ack:u8 |
 *                      session entries of {id:u8, type:u8, version:u8}
 *
 * Session identifiers are never hard-coded after startup. They are read
 * from the peer's SYN payload and are the identifiers used in both link
 * directions as this module's explicit negotiation policy. The reference
 * integration hard-codes 10/11/12 for sending and receiving; it does not prove
 * this module's handling of arbitrary peer ids. This module exposes
 * 10/11/12 only through its default local profile; peers may negotiate any
 * nonzero ids, and nonstandard ids remain unvalidated against real hardware.
 * Local advertisements only describe what this side offers during
 * negotiation. A data frame on an id absent from the negotiated table is
 * sequenced and ACKed but is not surfaced as application data
 * (rx_unknown_session counts it).
 *
 * The reference advertises max_len 65535 because its payload storage is
 * dynamically sized. That number is a reference implementation policy, not a
 * wire requirement. This module has fixed storage and therefore advertises
 * only its real receive bound (max_payload + 10; 4106 bytes by default).
 * Raise CP_LINK_MAX_PAYLOAD at build time if a deployment needs larger
 * control-session datagrams.
 *
 * Cumulative ACKs may be coalesced into an already queued, completely
 * unconsumed ACK-only frame. A frame whose prefix has been consumed is never
 * mutated; a new ACK is appended instead, and if the output queue is full the
 * ACK stays owed until the caller drains a frame. Output backpressure never
 * resets a healthy link for a redundant ACK.
 *
 * Outbound EAKs are bounded the same way. The missing list is recomputed from
 * the current reorder slots before every transmission, so it never requests a
 * packet that is already buffered and never retains a hole that has since been
 * closed. It is clamped to the peer's negotiated max_len (nine-byte header plus
 * one payload checksum byte) and to local frame limits. If the peer cannot
 * carry any EAK payload, a normal cumulative ACK is sent instead. At most one
 * completely unconsumed EAK frame is maintained; it is rewritten in place when
 * the holes change, cancelled if no holes remain, and deferred (never fatal)
 * when the output queue is full. A truncated EAK remembers the prefix it
 * requested: once that prefix changes because a hole filled, a follow-up EAK
 * requests the remaining holes without waiting for a new gap threshold.
 * Draining output or ticking alone never re-emits the same list. A partially
 * consumed EAK frame is immutable, as is a partially consumed ACK or data
 * frame.
 *
 * A DATA retransmission keeps its original sequence number, session id and
 * payload, but is rebuilt with the current cumulative ACK and a refreshed
 * header checksum. The stored original frame is not mutated.
 *
 * The passive role requires the exact six-byte marker before accepting a
 * link header. The initiator still emits the marker but, like the reference,
 * scans for 0xff5a immediately so a peer that answers with SYN before its own
 * marker is accepted. Both roles reject a wrong passive marker explicitly.
 *
 * CONTROL SESSION DATA IS RAW. A control-session frame is reported as a
 * CP_LINK_EVENT_SESSION_DATA whose `data` is exactly the link payload. In
 * particular this module does not parse, re-frame or synthesise 0x4040 CSM
 * headers. Authentication and identification state must live above this
 * module and be discarded whenever `generation` changes.
 *
 * All storage is fixed at compile time. No function calls malloc/free and
 * no operation loops without consuming input, advancing a deadline, or
 * terminating the link. Every transmit failure is either reported to the
 * caller (CP_LINK_ERR_BACKPRESSURE) or terminates the link with an explicit
 * reset reason; bytes are never silently dropped.
 *
 * Single-threaded use only. The event callback is invoked synchronously from
 * the API call that produced the event. Its `data` pointer is valid only for
 * the duration of the callback. Do not call back into cp_link_* from the
 * callback unless the integration is prepared for re-entrancy.
 */
#ifndef CP_LINK_H
#define CP_LINK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard buffer limits. They are compile-time constants; raising the payload
 * limit increases cp_link_t by roughly 20 bytes per payload byte (tx, reorder
 * and output storage all scale together). */
#ifndef CP_LINK_MAX_PAYLOAD
#define CP_LINK_MAX_PAYLOAD 4096u
#endif
#ifndef CP_LINK_MAX_OUTSTANDING
#define CP_LINK_MAX_OUTSTANDING 32u
#endif
#ifndef CP_LINK_MAX_REORDER
#define CP_LINK_MAX_REORDER 32u
#endif
#ifndef CP_LINK_OUTPUT_FRAMES
#define CP_LINK_OUTPUT_FRAMES 12u
#endif

#define CP_LINK_MARKER_LEN 6u
#define CP_LINK_HEADER_LEN 9u
#define CP_LINK_MAX_FRAME (CP_LINK_MAX_PAYLOAD + 10u)
#define CP_LINK_MAX_SESSIONS 8u
#define CP_LINK_MAX_EAK \
    (CP_LINK_MAX_PAYLOAD < 127u ? CP_LINK_MAX_PAYLOAD : 127u)

/* Two output slots are held back for protocol control (ACK/EAK/SYN/RST).
 * Application data is refused before it can consume the reserve. */
#define CP_LINK_OUTPUT_RESERVED 2u

/* Session type values used by the iAP2 link synchronization payload. */
#define CP_LINK_SESSION_CONTROL 0u
#define CP_LINK_SESSION_FILE_TRANSFER 1u
#define CP_LINK_SESSION_EXTERNAL_ACCESSORY 2u

#define CP_LINK_SESSION_ID_NONE (-1)
#define CP_LINK_DEFAULT_INITIAL_SEQ 0x63u

typedef enum cp_link_role {
    CP_LINK_ROLE_PASSIVE = 0,
    CP_LINK_ROLE_INITIATOR = 1
} cp_link_role_t;

typedef enum cp_link_state {
    CP_LINK_STATE_IDLE = 0,
    CP_LINK_STATE_DETECT,
    CP_LINK_STATE_NEGOTIATE,
    CP_LINK_STATE_NORMAL,
    CP_LINK_STATE_DEAD
} cp_link_state_t;

typedef enum cp_link_result {
    CP_LINK_OK = 0,
    CP_LINK_ERR_STATE = -1,
    CP_LINK_ERR_BACKPRESSURE = -2,
    CP_LINK_ERR_TOO_LARGE = -3,
    CP_LINK_ERR_NO_SESSION = -4,
    CP_LINK_ERR_INVALID = -5
} cp_link_result_t;

typedef enum cp_link_reset_reason {
    CP_LINK_RESET_NONE = 0,
    CP_LINK_RESET_CANCELLED,
    CP_LINK_RESET_LOCAL_ABORT,
    CP_LINK_RESET_MARKER,
    CP_LINK_RESET_PEER,
    CP_LINK_RESET_HANDSHAKE_TIMEOUT,
    CP_LINK_RESET_DETECT_EXHAUSTED,
    CP_LINK_RESET_NEGOTIATE_EXHAUSTED,
    CP_LINK_RESET_ACK_TIMEOUT,
    CP_LINK_RESET_SYNC_INVALID,
    CP_LINK_RESET_SESSION_UNSUPPORTED,
    CP_LINK_RESET_LIMITS,
    CP_LINK_RESET_TX_BACKPRESSURE,
    CP_LINK_RESET_INTERNAL
} cp_link_reset_reason_t;

typedef enum cp_link_event_type {
    CP_LINK_EVENT_STATE = 1,
    CP_LINK_EVENT_WRITABLE,
    CP_LINK_EVENT_SESSION_DATA,
    CP_LINK_EVENT_RESET
} cp_link_event_type_t;

typedef struct cp_link_session {
    uint8_t id;
    uint8_t type;
    uint8_t version;
} cp_link_session_t;

typedef struct cp_link_sync {
    uint8_t version;
    uint8_t max_outgoing;
    uint16_t max_len; /* maximum received wire packet size, header included */
    uint16_t retransmission_timeout_ms;
    uint16_t ack_timeout_ms;
    uint8_t max_retransmissions;
    uint8_t max_ack;
    uint8_t session_count;
    cp_link_session_t sessions[CP_LINK_MAX_SESSIONS];
} cp_link_sync_t;

typedef struct cp_link_config {
    uint8_t initial_seq;

    /* Locally advertised sessions. Set a secondary id to zero to omit it.
     * The control session is mandatory. */
    uint8_t control_session_id;
    uint8_t external_accessory_session_id;
    uint8_t file_transfer_session_id;
    uint8_t control_version;

    uint8_t max_outgoing;
    uint16_t max_payload;
    uint16_t retransmission_timeout_ms;
    uint16_t ack_timeout_ms;
    uint8_t max_retransmissions;
    uint8_t max_ack;

    /* ZeroACK/zero-retransmit mode: advertise all four flow-control values as
     * zero, matching the wired reference configuration. */
    bool zero_ack;

    /* Bounded detection and negotiation retries. The absolute handshake
     * deadline is a second, independent bound. */
    uint16_t detect_interval_ms;
    uint8_t detect_retry_limit;
    uint16_t negotiate_interval_ms;
    uint8_t negotiate_retry_limit;
    uint16_t handshake_timeout_ms;
} cp_link_config_t;

typedef struct cp_link_event {
    cp_link_event_type_t type;
    cp_link_state_t state;
    bool writable;
    cp_link_reset_reason_t reset_reason;
    uint32_t generation;
    uint8_t session_id;
    uint8_t session_type;
    const uint8_t *data;
    uint16_t data_len;
} cp_link_event_t;

typedef void (*cp_link_event_fn)(void *user, const cp_link_event_t *event);

typedef struct cp_link_stats {
    uint32_t bytes_fed;
    uint32_t frames_valid;
    uint32_t frames_bad_header;
    uint32_t frames_bad_payload;
    uint32_t frames_bad_length;
    uint32_t resync_bytes;
    uint32_t marker_sent;
    uint32_t marker_received;
    uint32_t syn_sent;
    uint32_t syn_received;
    uint32_t syn_invalid;
    uint32_t syn_ignored;
    uint32_t ack_sent;
    uint32_t ack_received;
    uint32_t ack_ignored;
    uint32_t eak_sent;
    uint32_t eak_received;
    uint32_t data_sent;
    uint32_t data_received;
    uint32_t data_ignored;
    uint32_t duplicates;
    uint32_t out_of_order;
    uint32_t out_of_window;
    uint32_t reorder_full;
    uint32_t retransmits;
    uint32_t tx_backpressure;
    uint32_t rx_unknown_session;
    uint32_t reset_count;
} cp_link_stats_t;

/* Internal fixed-size records. They are visible so callers can allocate the
 * engine statically; treat them as private. At the default limits cp_link_t
 * is roughly 300 KiB, so allocate it once in static or caller-owned storage
 * rather than on a small task stack. */
typedef struct cp_link_header {
    uint16_t length;
    uint8_t control;
    uint8_t seq;
    uint8_t ack;
    uint8_t session_id;
} cp_link_header_t;

typedef struct cp_link_out_slot {
    uint16_t len;
    uint16_t consumed;
    uint8_t bytes[CP_LINK_MAX_FRAME];
} cp_link_out_slot_t;

typedef struct cp_link_tx_slot {
    bool used;
    uint8_t psn;
    uint8_t session_id;
    uint8_t retries;
    uint64_t deadline_ms;
    uint16_t len;
    uint8_t frame[CP_LINK_MAX_FRAME];
} cp_link_tx_slot_t;

typedef struct cp_link_rx_slot {
    bool used;
    uint8_t psn;
    uint8_t session_id;
    uint16_t len;
    uint8_t data[CP_LINK_MAX_PAYLOAD];
} cp_link_rx_slot_t;

typedef enum cp_link_rx_phase {
    CP_LINK_RX_MARKER = 0,
    CP_LINK_RX_HEADER,
    CP_LINK_RX_PAYLOAD
} cp_link_rx_phase_t;

typedef struct cp_link {
    /* Private state. Do not modify fields directly. */
    cp_link_config_t cfg;
    cp_link_sync_t local_sync;
    cp_link_sync_t peer_sync;
    cp_link_event_fn on_event;
    void *user;

    cp_link_state_t state;
    cp_link_reset_reason_t reset_reason;
    uint32_t generation;
    cp_link_stats_t stats;
    bool initialized;

    uint8_t initial_seq;
    uint8_t sent_psn;
    uint8_t peer_ack;
    bool peer_ack_valid;
    uint8_t last_received_psn;
    uint8_t last_acked_psn;
    bool last_acked_valid;
    bool peer_sync_valid;
    bool syn_sent;
    bool writable;
    bool tx_blocked;

    uint32_t detect_retries;
    uint32_t syn_retries;
    uint64_t detect_deadline_ms;
    uint64_t negotiate_deadline_ms;
    uint64_t handshake_deadline_ms;
    uint64_t delayed_ack_deadline_ms;
    bool delayed_ack_armed;
    bool ack_due;
    bool eak_due;
    bool eak_more;
    /* Last truncated EAK prefix, used for progress-only follow-ups. */
    uint8_t eak_last_count;
    uint8_t eak_last_missing[CP_LINK_MAX_EAK];
    uint64_t now_ms;

    uint16_t syn_len;
    uint8_t syn_frame[CP_LINK_MAX_FRAME];

    cp_link_out_slot_t out_slots[CP_LINK_OUTPUT_FRAMES];
    uint8_t out_head;
    uint8_t out_count;

    cp_link_tx_slot_t tx_slots[CP_LINK_MAX_OUTSTANDING];
    uint8_t tx_count;

    cp_link_rx_slot_t rx_slots[CP_LINK_MAX_REORDER];
    uint8_t rx_count;

    cp_link_rx_phase_t rx_phase;
    uint8_t marker_buf[CP_LINK_MARKER_LEN];
    uint8_t marker_len;
    uint8_t header_buf[CP_LINK_HEADER_LEN];
    uint8_t header_len;
    cp_link_header_t current_header;
    uint16_t payload_need;
    uint16_t payload_len;
    uint8_t payload_buf[CP_LINK_MAX_FRAME];

    uint16_t cumulative_received;
} cp_link_t;

/* Fill `cfg` with bounded local defaults. These are a conservative local
 * policy (control-session version 1, max_outgoing 30 clamped by
 * CP_LINK_MAX_OUTSTANDING, and local ids 10/11/12), not a live-validated
 * profile; keep the ids only as an offer. Use
 * cp_link_config_live_bluetooth() for the profile observed on the reference
 * Bluetooth link. */
void cp_link_config_default(cp_link_config_t *cfg);

/* Fill `cfg` with the profile observed on the live LIVI Bluetooth link:
 * control-session version 2, max_outgoing 4 and ordinary (non-ZeroACK) flow
 * control. The emitted SYN is pinned by a fixed vector in the tests. */
void cp_link_config_live_bluetooth(cp_link_config_t *cfg);

/* Initialise a caller-owned engine. `on_event` is required. Returns
 * CP_LINK_OK, or CP_LINK_ERR_INVALID for a bad config/callback. */
int cp_link_init(cp_link_t *link, const cp_link_config_t *cfg,
                 cp_link_event_fn on_event, void *user);

/* Start detection. The initiator emits the marker and a SYN immediately; the
 * passive side emits the marker and waits for the peer marker before its SYN.
 * A new start invalidates the previous generation. */
int cp_link_start(cp_link_t *link, cp_link_role_t role, uint64_t now_ms);

/* Consume arbitrary transport bytes. Fragmentation, coalescing and junk
 * between 0xff5a headers are supported. Returns CP_LINK_ERR_STATE only when
 * the engine was idle or already dead on entry. */
int cp_link_feed(cp_link_t *link, const void *data, size_t len, uint64_t now_ms);

/* Service deadlines (detection, negotiation, delayed ACK and retransmit).
 * Safe to call at any time; a no-op when no deadline is due. */
int cp_link_tick(cp_link_t *link, uint64_t now_ms);

/* Queue one session payload. `session_kind` is CP_LINK_SESSION_CONTROL,
 * _FILE_TRANSFER or _EXTERNAL_ACCESSORY and is resolved only against the
 * peer's negotiated SYN table. Returns CP_LINK_ERR_BACKPRESSURE when the
 * caller has not drained output, the peer window is full, or a deferred
 * retransmission backlog is waiting for output space; the caller must retry
 * later. No application bytes are queued while backpressured. */
int cp_link_send(cp_link_t *link, uint8_t session_kind, const void *data,
                 size_t len, uint64_t now_ms);

/* Queue one payload on an explicit negotiated session identifier. The id must
 * be present in the peer's SYN table; arbitrary/unnegotiated ids are refused.
 * This is the advanced path for transports that already demultiplex by id. */
int cp_link_send_session(cp_link_t *link, uint8_t session_id, const void *data,
                         size_t len, uint64_t now_ms);

/* Local cancellation: no wire bytes, immediately dead, queues cleared,
 * generation advanced. The caller must tear down any auth context bound to
 * the old generation. */
int cp_link_cancel(cp_link_t *link, cp_link_reset_reason_t reason);

/* Local abort: queue exactly one RST (if output is free), then die. Queues are
 * cleared first; the RST is the only remaining output frame. */
int cp_link_abort(cp_link_t *link, uint64_t now_ms,
                  cp_link_reset_reason_t reason);

/* Output queue inspection. The returned pointer is the *unconsumed* suffix of
 * the oldest queued frame. It remains valid until the next mutating call.
 * A transport may write any prefix and then consume exactly those bytes.
 * A completely unconsumed ACK-only frame may be rewritten in place when a
 * cumulative ACK is coalesced, and a completely unconsumed EAK frame may be
 * rewritten in place when its missing list changes or retracted when it has no
 * holes left; a consumed prefix is never modified.
 * If a retransmission burst does not fit, the remaining retransmissions stay
 * pending (they are never discarded) and resume as output slots are consumed.
 * Queued bytes are not transport-confirmed: drain them promptly so that
 * retransmission and delayed-ACK deadlines reflect on-wire progress. A start,
 * cancel, abort or peer reset advances `generation` and may discard a
 * partially written frame; the transport must reset or resynchronise its byte
 * stream whenever generation changes. */
size_t cp_link_output_peek(const cp_link_t *link, const uint8_t **data);
size_t cp_link_output_pending(const cp_link_t *link);
int cp_link_output_consume(cp_link_t *link, size_t len);

/* Earliest deadline in the caller's monotonic millisecond domain, or
 * UINT64_MAX when none is armed. */
uint64_t cp_link_next_deadline(const cp_link_t *link);

cp_link_state_t cp_link_state(const cp_link_t *link);
bool cp_link_writable(const cp_link_t *link);
uint32_t cp_link_generation(const cp_link_t *link);
cp_link_reset_reason_t cp_link_reset_reason(const cp_link_t *link);
const char *cp_link_reset_reason_string(cp_link_reset_reason_t reason);

/* Resolve a known session type to the peer's negotiated identifier, or
 * CP_LINK_SESSION_ID_NONE. Unknown/unnegotiated types are not interpreted. */
int cp_link_session_id(const cp_link_t *link, uint8_t session_kind);

const cp_link_sync_t *cp_link_local_sync(const cp_link_t *link);
const cp_link_sync_t *cp_link_peer_sync(const cp_link_t *link);
size_t cp_link_max_payload(const cp_link_t *link);
const cp_link_stats_t *cp_link_stats(const cp_link_t *link);

/* Serial-number arithmetic: forward distance from `older` to `newer`,
 * modulo 256. */
uint8_t cp_link_sequence_distance(uint8_t newer, uint8_t older);

#ifdef __cplusplus
}
#endif

#endif /* CP_LINK_H */
