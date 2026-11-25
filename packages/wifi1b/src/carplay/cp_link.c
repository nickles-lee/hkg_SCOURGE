/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded iAP2 link engine. See cp_link.h for the wire facts and contract.
 * No sockets, clocks, hardware access, dynamic allocation or CSM parsing.
 */
#include "cp_link.h"

#include <string.h>

#define CP_LINK_CONTROL_SYN 0x80u
#define CP_LINK_CONTROL_ACK 0x40u
#define CP_LINK_CONTROL_EAK 0x20u
#define CP_LINK_CONTROL_RST 0x10u

#define CP_LINK_SYNC_VERSION 0x01u
#define CP_LINK_MAX_RETRY_LIMIT 32u
#define CP_LINK_RX_WINDOW_SLACK 10u

static const uint8_t cp_link_marker[CP_LINK_MARKER_LEN] = {
    0xFFu, 0x55u, 0x02u, 0x00u, 0xEEu, 0x10u
};

_Static_assert(CP_LINK_MAX_PAYLOAD >= 16u, "payload limit");
_Static_assert(CP_LINK_MAX_PAYLOAD <= 65525u, "wire length must fit u16");
_Static_assert(CP_LINK_MAX_OUTSTANDING >= 1u, "tx slots");
_Static_assert(CP_LINK_MAX_OUTSTANDING <= 255u, "tx count is u8");
_Static_assert(CP_LINK_MAX_REORDER >= 1u, "rx slots");
_Static_assert(CP_LINK_MAX_REORDER <= 255u, "reorder count is u8");
_Static_assert(CP_LINK_OUTPUT_FRAMES > CP_LINK_OUTPUT_RESERVED + 1u, "output reserve");
_Static_assert(CP_LINK_OUTPUT_FRAMES <= 255u, "output count is u8");
_Static_assert(CP_LINK_MAX_SESSIONS >= 3u, "local control/EA/FT session slots");
_Static_assert(CP_LINK_MAX_SESSIONS <= 255u, "session count is u8");
_Static_assert(CP_LINK_MAX_EAK <= 255u, "EAK payload is u8 PSN list");

static bool retransmit_due(cp_link_t *link, uint64_t now_ms);

static uint8_t sum8(const uint8_t *data, size_t len)
{
    uint8_t sum = 0;
    size_t i;

    for (i = 0; i < len; i++)
        sum = (uint8_t)(sum + data[i]);
    return sum;
}

static uint8_t checksum8(const uint8_t *data, size_t len)
{
    unsigned sum = (unsigned)sum8(data, len);

    return (uint8_t)((256u - sum) & 0xFFu);
}

static uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static void put_be16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)(value & 0xFFu);
}

static uint64_t add_ms(uint64_t now, uint32_t delta)
{
    if (UINT64_MAX - now < (uint64_t)delta)
        return UINT64_MAX;
    return now + (uint64_t)delta;
}

static uint64_t deadline_min(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

uint8_t cp_link_sequence_distance(uint8_t newer, uint8_t older)
{
    return (uint8_t)(newer - older);
}

static void clear_output(cp_link_t *link)
{
    memset(link->out_slots, 0, sizeof(link->out_slots));
    link->out_head = 0;
    link->out_count = 0;
}

static void clear_tx(cp_link_t *link)
{
    memset(link->tx_slots, 0, sizeof(link->tx_slots));
    link->tx_count = 0;
}

static void clear_rx(cp_link_t *link)
{
    memset(link->rx_slots, 0, sizeof(link->rx_slots));
    link->rx_count = 0;
}

static void clear_peer_sync(cp_link_t *link)
{
    memset(&link->peer_sync, 0, sizeof(link->peer_sync));
    link->peer_sync_valid = false;
}

static void clear_parser(cp_link_t *link)
{
    link->marker_len = 0;
    link->header_len = 0;
    link->payload_need = 0;
    link->payload_len = 0;
    memset(link->marker_buf, 0, sizeof(link->marker_buf));
    memset(link->header_buf, 0, sizeof(link->header_buf));
    memset(&link->current_header, 0, sizeof(link->current_header));
    memset(link->payload_buf, 0, sizeof(link->payload_buf));
}

static void clear_deadlines(cp_link_t *link)
{
    link->detect_deadline_ms = 0;
    link->negotiate_deadline_ms = 0;
    link->handshake_deadline_ms = 0;
    link->delayed_ack_deadline_ms = 0;
    link->delayed_ack_armed = false;
    link->ack_due = false;
    link->eak_due = false;
    link->eak_more = false;
    link->eak_last_count = 0u;
}

static void emit_event(cp_link_t *link, const cp_link_event_t *event)
{
    if (link->on_event != NULL)
        link->on_event(link->user, event);
}

static void emit_state(cp_link_t *link)
{
    cp_link_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CP_LINK_EVENT_STATE;
    event.state = link->state;
    event.generation = link->generation;
    event.writable = link->writable;
    emit_event(link, &event);
}

static void emit_writable(cp_link_t *link, bool writable)
{
    cp_link_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CP_LINK_EVENT_WRITABLE;
    event.state = link->state;
    event.generation = link->generation;
    event.writable = writable;
    emit_event(link, &event);
}

static void emit_reset(cp_link_t *link)
{
    cp_link_event_t event;

    memset(&event, 0, sizeof(event));
    event.type = CP_LINK_EVENT_RESET;
    event.state = link->state;
    event.generation = link->generation;
    event.reset_reason = link->reset_reason;
    event.writable = false;
    emit_event(link, &event);
}

static bool can_send_now(const cp_link_t *link)
{
    if (link->state != CP_LINK_STATE_NORMAL || link->tx_blocked)
        return false;
    if ((unsigned)link->out_count + CP_LINK_OUTPUT_RESERVED >=
        (unsigned)CP_LINK_OUTPUT_FRAMES)
        return false;
    if (!link->peer_sync_valid)
        return false;
    if (link->peer_sync.max_retransmissions > 0) {
        if (link->peer_sync.max_outgoing == 0)
            return false;
        if (link->tx_count >= link->peer_sync.max_outgoing)
            return false;
    }
    return true;
}

static void update_writable(cp_link_t *link)
{
    bool writable = can_send_now(link);

    if (writable != link->writable) {
        link->writable = writable;
        emit_writable(link, writable);
    }
}

/*
 * Terminate once, clear every record that could carry authentication or
 * sequence state, advance the generation and report explicit reasons.
 * When keep_output is true the caller has already cleared the queue and
 * queued exactly the final RST frame.
 */
static void terminate_link(cp_link_t *link, cp_link_reset_reason_t reason,
                           bool keep_output)
{
    bool was_writable;

    if (link->state == CP_LINK_STATE_DEAD)
        return;

    was_writable = link->writable;
    link->state = CP_LINK_STATE_DEAD;
    link->reset_reason =
        reason == CP_LINK_RESET_NONE ? CP_LINK_RESET_INTERNAL : reason;
    link->generation++;
    link->stats.reset_count++;

    clear_deadlines(link);
    clear_tx(link);
    clear_rx(link);
    clear_peer_sync(link);
    clear_parser(link);
    if (!keep_output)
        clear_output(link);

    link->peer_ack_valid = false;
    link->last_acked_valid = false;
    link->syn_sent = false;
    link->syn_len = 0;
    link->sent_psn = 0;
    link->peer_ack = 0;
    link->last_received_psn = 0;
    link->last_acked_psn = 0;
    link->cumulative_received = 0;
    link->detect_retries = 0;
    link->syn_retries = 0;
    link->writable = false;
    link->tx_blocked = false;
    memset(link->syn_frame, 0, sizeof(link->syn_frame));

    if (was_writable)
        emit_writable(link, false);
    emit_reset(link);
    emit_state(link);
}

static size_t build_frame(uint8_t *out, size_t cap, uint8_t control,
                          uint8_t seq, uint8_t ack, uint8_t session_id,
                          bool has_payload, const uint8_t *payload,
                          size_t payload_len)
{
    size_t total;

    if (has_payload && payload == NULL && payload_len != 0)
        return 0;
    total = CP_LINK_HEADER_LEN + (has_payload ? payload_len + 1u : 0u);
    if (total > cap || total > 0xFFFFu)
        return 0;
    if (has_payload && payload_len > CP_LINK_MAX_PAYLOAD)
        return 0;

    out[0] = 0xFFu;
    out[1] = 0x5Au;
    put_be16(out + 2, (uint16_t)total);
    out[4] = control;
    out[5] = seq;
    out[6] = ack;
    out[7] = session_id;
    out[8] = checksum8(out, 8u);

    if (has_payload) {
        if (payload_len != 0)
            memcpy(out + CP_LINK_HEADER_LEN, payload, payload_len);
        out[CP_LINK_HEADER_LEN + payload_len] =
            checksum8(payload, payload_len);
    }
    return total;
}

static bool output_enqueue(cp_link_t *link, const uint8_t *bytes, size_t len)
{
    unsigned index;
    cp_link_out_slot_t *slot;

    if (bytes == NULL || len == 0 || len > CP_LINK_MAX_FRAME)
        return false;
    if (link->out_count >= CP_LINK_OUTPUT_FRAMES)
        return false;

    index = ((unsigned)link->out_head + (unsigned)link->out_count) %
            (unsigned)CP_LINK_OUTPUT_FRAMES;
    slot = &link->out_slots[index];
    memmove(slot->bytes, bytes, len);
    slot->len = (uint16_t)len;
    slot->consumed = 0;
    link->out_count++;
    return true;
}

static bool enqueue_control(cp_link_t *link, uint8_t control,
                            const uint8_t *payload, size_t payload_len,
                            bool has_payload)
{
    uint8_t frame[CP_LINK_MAX_FRAME];
    size_t frame_len;

    frame_len = build_frame(frame, sizeof(frame), control, link->sent_psn,
                            link->last_received_psn, 0u, has_payload, payload,
                            payload_len);
    if (frame_len == 0)
        return false;
    return output_enqueue(link, frame, frame_len);
}

static bool output_tail(cp_link_t *link, cp_link_out_slot_t **slot)
{
    unsigned index;

    if (link->out_count == 0u)
        return false;
    index = ((unsigned)link->out_head + (unsigned)link->out_count - 1u) %
            (unsigned)CP_LINK_OUTPUT_FRAMES;
    *slot = &link->out_slots[index];
    return true;
}

/*
 * Remove one completely unconsumed queued frame. Frames behind an unconsumed
 * frame cannot have been emitted (the queue is FIFO), so compacting them is
 * safe. Only protocol control frames that are safe to retract use this.
 */
static void output_remove_at(cp_link_t *link, unsigned offset)
{
    unsigned i;

    if (offset >= link->out_count)
        return;
    for (i = offset; i + 1u < link->out_count; i++) {
        unsigned dst = ((unsigned)link->out_head + i) %
                       (unsigned)CP_LINK_OUTPUT_FRAMES;
        unsigned src = ((unsigned)link->out_head + i + 1u) %
                       (unsigned)CP_LINK_OUTPUT_FRAMES;

        link->out_slots[dst] = link->out_slots[src];
    }
    link->out_count--;
    link->tx_blocked = false;
}

/*
 * Fold the current cumulative acknowledgement into the newest queued frame
 * when it is an ACK-only frame with no emitted prefix. A partially consumed
 * frame is immutable; callers append a new ACK instead. Returns true when the
 * queued frame was updated.
 */
static bool coalesce_ack(cp_link_t *link)
{
    cp_link_out_slot_t *slot;

    if (!output_tail(link, &slot))
        return false;
    if (slot->consumed != 0u || slot->len != CP_LINK_HEADER_LEN)
        return false;
    if (slot->bytes[4] != CP_LINK_CONTROL_ACK)
        return false;
    slot->bytes[5] = link->sent_psn;
    slot->bytes[6] = link->last_received_psn;
    slot->bytes[8] = checksum8(slot->bytes, CP_LINK_HEADER_LEN - 1u);
    return true;
}

/*
 * Queue the cumulative ACK, coalescing into an unconsumed ACK-only tail
 * frame. If the output queue is full, remember that the ACK is still owed and
 * let output_consume()/tick() retry once space exists. A redundant ACK must
 * never reset a healthy link.
 */
static void send_ack(cp_link_t *link)
{
    bool coalesced;
    bool queued;

    if (link->state == CP_LINK_STATE_DEAD)
        return;

    coalesced = coalesce_ack(link);
    queued = coalesced ||
             enqueue_control(link, CP_LINK_CONTROL_ACK, NULL, 0, false);
    if (!queued) {
        link->ack_due = true;
        link->delayed_ack_armed = false;
        link->delayed_ack_deadline_ms = 0;
        return;
    }

    link->ack_due = false;
    link->last_acked_psn = link->last_received_psn;
    link->last_acked_valid = true;
    link->delayed_ack_armed = false;
    link->delayed_ack_deadline_ms = 0;
    link->cumulative_received = 0;
    if (!coalesced)
        link->stats.ack_sent++;
}

static bool rx_has_psn(const cp_link_t *link, uint8_t psn)
{
    uint8_t i;

    for (i = 0; i < CP_LINK_MAX_REORDER; i++) {
        if (link->rx_slots[i].used && link->rx_slots[i].psn == psn)
            return true;
    }
    return false;
}

/*
 * Recompute the currently known holes from the reorder buffer. The frontier is
 * the furthest buffered sequence; every unbuffered sequence between the last
 * contiguous packet and that frontier is missing. Already buffered packets and
 * holes closed by last_received_psn advancing are never included.
 */
static size_t current_missing(const cp_link_t *link, uint8_t *missing,
                              size_t cap)
{
    uint8_t frontier = 0u;
    bool have_frontier = false;
    uint8_t i;
    uint16_t offset;
    size_t count = 0u;

    if (missing == NULL || cap == 0u)
        return 0u;

    for (i = 0; i < CP_LINK_MAX_REORDER; i++) {
        const cp_link_rx_slot_t *slot = &link->rx_slots[i];
        uint8_t distance;

        if (!slot->used)
            continue;
        distance = cp_link_sequence_distance(slot->psn,
                                             link->last_received_psn);
        if (distance < 2u || distance >= 128u)
            continue;
        if (!have_frontier || distance > frontier) {
            frontier = distance;
            have_frontier = true;
        }
    }
    if (!have_frontier)
        return 0u;

    for (offset = 1u; offset < (uint16_t)frontier && count < cap; offset++) {
        uint8_t psn = (uint8_t)((unsigned)link->last_received_psn + offset);

        if (!rx_has_psn(link, psn))
            missing[count++] = psn;
    }
    return count;
}

/*
 * Largest EAK payload the peer can receive. The wire frame is a 9-byte header
 * plus the missing list plus one payload checksum byte, so a peer max_len of
 * 9 or 10 cannot carry any EAK payload. The result is also bounded by this
 * module's local frame/EAK limits.
 */
static size_t eak_payload_capacity(const cp_link_t *link)
{
    size_t capacity;

    if (!link->peer_sync_valid ||
        link->peer_sync.max_len <= CP_LINK_HEADER_LEN + 1u)
        return 0u;
    capacity = (size_t)link->peer_sync.max_len - CP_LINK_HEADER_LEN - 1u;
    if (capacity > (size_t)CP_LINK_MAX_EAK)
        capacity = (size_t)CP_LINK_MAX_EAK;
    if (capacity > (size_t)CP_LINK_MAX_PAYLOAD)
        capacity = (size_t)CP_LINK_MAX_PAYLOAD;
    return capacity;
}

/*
 * Remember the truncated prefix that was just queued/rewritten. The stored
 * list lets a consumed truncated EAK request only the remaining holes after a
 * packet changes that prefix, without emitting the same list again merely for
 * consuming output or ticking.
 */
static void eak_set_more(cp_link_t *link, const uint8_t *missing, size_t count,
                         size_t available)
{
    if (count < available) {
        link->eak_more = true;
        link->eak_last_count = (uint8_t)count;
        if (count != 0u)
            memcpy(link->eak_last_missing, missing, count);
    } else {
        link->eak_more = false;
        link->eak_last_count = 0u;
    }
}

static bool eak_matches_last(const cp_link_t *link, const uint8_t *missing,
                             size_t count)
{
    if (!link->eak_more || count != (size_t)link->eak_last_count)
        return false;
    return count == 0u ||
           memcmp(link->eak_last_missing, missing, count) == 0;
}

/* Return the queue offset of a completely unconsumed EAK, or -1. */
static int find_unconsumed_eak(const cp_link_t *link)
{
    unsigned i;

    for (i = 0; i < link->out_count; i++) {
        unsigned index = ((unsigned)link->out_head + i) %
                         (unsigned)CP_LINK_OUTPUT_FRAMES;
        const cp_link_out_slot_t *slot = &link->out_slots[index];

        if (slot->consumed == 0u && slot->len > CP_LINK_HEADER_LEN &&
            slot->bytes[4] == CP_LINK_CONTROL_EAK)
            return (int)i;
    }
    return -1;
}

static bool eak_rewrite(cp_link_t *link, cp_link_out_slot_t *slot,
                        const uint8_t *missing, size_t count)
{
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t seq = slot->bytes[5];
    size_t frame_len;

    frame_len = build_frame(frame, sizeof(frame), CP_LINK_CONTROL_EAK,
                            link->sent_psn, link->last_received_psn, 0u, true,
                            missing, count);
    if (frame_len == 0)
        return false;
    /* Keep the queued frame's sequence position even if application data was
     * queued behind it; only the ACK field, payload and checksum change. */
    frame[5] = seq;
    frame[8] = checksum8(frame, CP_LINK_HEADER_LEN - 1u);
    memmove(slot->bytes, frame, frame_len);
    slot->len = (uint16_t)frame_len;
    slot->consumed = 0;
    return true;
}

/*
 * Bring the outstanding EAK in line with the current reorder state. No EAK is
 * created unless one was requested (a gap threshold was crossed) or one is
 * already queued and unconsumed. A queued EAK is rewritten in place, and an
 * EAK whose holes all closed is retracted. If output is full the request stays
 * pending for output_consume()/tick(); it never resets a healthy link.
 * A partially consumed EAK is immutable and is deliberately not rewritten;
 * a later threshold crossing may append a fresh unconsumed EAK after it.
 */
static void eak_refresh(cp_link_t *link)
{
    uint8_t missing[CP_LINK_MAX_EAK];
    size_t available;
    size_t capacity;
    size_t count;
    int offset;

    if (link->state == CP_LINK_STATE_DEAD) {
        link->eak_due = false;
        return;
    }

    offset = find_unconsumed_eak(link);
    if (!link->eak_due && offset < 0 && !link->eak_more)
        return;

    available = current_missing(link, missing, sizeof(missing));
    if (available == 0u) {
        if (offset >= 0) {
            output_remove_at(link, (unsigned)offset);
            if (link->ack_due)
                send_ack(link);
            update_writable(link);
        }
        link->eak_due = false;
        link->eak_more = false;
        link->eak_last_count = 0u;
        return;
    }

    capacity = eak_payload_capacity(link);
    if (capacity == 0u) {
        /* The peer cannot carry even a one-byte EAK payload. Acknowledge the
         * received sequence with an ordinary 9-byte cumulative ACK instead of
         * emitting an oversized EAK or resetting the link. */
        if (offset >= 0)
            output_remove_at(link, (unsigned)offset);
        link->eak_due = false;
        link->eak_more = false;
        link->eak_last_count = 0u;
        send_ack(link);
        update_writable(link);
        return;
    }

    count = available;
    if (count > capacity)
        count = capacity;

    /* A consumed truncated EAK is followed up only when the current clamped
     * prefix differs from the one already requested. Consume/tick do not call
     * this function without eak_due, so no same-list EAK results merely from
     * draining output. A repeated threshold that would only re-request the
     * same prefix is also absorbed. */
    if (offset < 0 && eak_matches_last(link, missing, count)) {
        link->eak_due = false;
        return;
    }

    if (offset >= 0) {
        unsigned index = ((unsigned)link->out_head + (unsigned)offset) %
                         (unsigned)CP_LINK_OUTPUT_FRAMES;
        cp_link_out_slot_t *slot = &link->out_slots[index];

        if (!eak_rewrite(link, slot, missing, count)) {
            link->eak_due = true;
            return;
        }
        eak_set_more(link, missing, count, available);
        link->eak_due = false;
        return;
    }

    if (!enqueue_control(link, CP_LINK_CONTROL_EAK, missing, count, true)) {
        link->eak_due = true;
        return;
    }

    /* A newly queued EAK uses the current ACK and, as before, closes the
     * delayed-ACK window. ack_due is left untouched so a deferred standalone
     * ACK is still delivered; rewriting an existing EAK also leaves all
     * delayed-ACK bookkeeping alone. */
    link->eak_due = false;
    link->delayed_ack_armed = false;
    link->delayed_ack_deadline_ms = 0;
    link->cumulative_received = 0;
    eak_set_more(link, missing, count, available);
    link->stats.eak_sent++;
}

static void send_marker(cp_link_t *link, uint64_t now_ms)
{
    if (!output_enqueue(link, cp_link_marker, sizeof(cp_link_marker))) {
        terminate_link(link, CP_LINK_RESET_TX_BACKPRESSURE, false);
        return;
    }
    link->stats.marker_sent++;
    if (link->state == CP_LINK_STATE_DETECT)
        link->detect_deadline_ms = add_ms(now_ms, link->cfg.detect_interval_ms);
}

static size_t sync_encode(const cp_link_sync_t *sync, uint8_t *out,
                          size_t cap)
{
    size_t need = 10u + (size_t)sync->session_count * 3u;
    size_t i;

    if (need > cap || sync->session_count > CP_LINK_MAX_SESSIONS)
        return 0;

    out[0] = CP_LINK_SYNC_VERSION;
    out[1] = sync->max_outgoing;
    put_be16(out + 2, sync->max_len);
    put_be16(out + 4, sync->retransmission_timeout_ms);
    put_be16(out + 6, sync->ack_timeout_ms);
    out[8] = sync->max_retransmissions;
    out[9] = sync->max_ack;
    for (i = 0; i < sync->session_count; i++) {
        out[10u + i * 3u] = sync->sessions[i].id;
        out[11u + i * 3u] = sync->sessions[i].type;
        out[12u + i * 3u] = sync->sessions[i].version;
    }
    return need;
}

static bool sync_parse(const uint8_t *payload, size_t len, cp_link_sync_t *out)
{
    cp_link_sync_t parsed;
    size_t count;
    size_t i;
    unsigned control_count = 0;

    if (payload == NULL || len < 10u || payload[0] != CP_LINK_SYNC_VERSION)
        return false;
    if ((len - 10u) % 3u != 0u)
        return false;
    count = (len - 10u) / 3u;
    if (count > CP_LINK_MAX_SESSIONS)
        return false;

    memset(&parsed, 0, sizeof(parsed));
    parsed.version = payload[0];
    parsed.max_outgoing = payload[1];
    parsed.max_len = get_be16(payload + 2);
    parsed.retransmission_timeout_ms = get_be16(payload + 4);
    parsed.ack_timeout_ms = get_be16(payload + 6);
    parsed.max_retransmissions = payload[8];
    parsed.max_ack = payload[9];
    parsed.session_count = (uint8_t)count;

    if (parsed.max_len < CP_LINK_HEADER_LEN)
        return false;
    /* A zero retransmission timer with retries enabled would busy-loop. */
    if (parsed.max_retransmissions > 0 && parsed.retransmission_timeout_ms == 0)
        return false;

    for (i = 0; i < count; i++) {
        cp_link_session_t session;
        size_t j;

        session.id = payload[10u + i * 3u];
        session.type = payload[11u + i * 3u];
        session.version = payload[12u + i * 3u];
        if (session.id == 0u)
            return false; /* 0 is the link session, never a data session */
        for (j = 0; j < i; j++) {
            if (parsed.sessions[j].id == session.id)
                return false;
        }
        if (session.type == CP_LINK_SESSION_CONTROL) {
            control_count++;
            if (control_count > 1u)
                return false;
        }
        parsed.sessions[i] = session;
    }

    *out = parsed;
    return true;
}

static int session_id_for_type(const cp_link_sync_t *sync, uint8_t type)
{
    uint8_t i;

    if (sync == NULL)
        return CP_LINK_SESSION_ID_NONE;
    for (i = 0; i < sync->session_count; i++) {
        if (sync->sessions[i].type == type)
            return (int)sync->sessions[i].id;
    }
    return CP_LINK_SESSION_ID_NONE;
}

static int session_type_for_id(const cp_link_sync_t *sync, uint8_t id)
{
    uint8_t i;

    if (sync == NULL)
        return -1;
    for (i = 0; i < sync->session_count; i++) {
        if (sync->sessions[i].id == id)
            return (int)sync->sessions[i].type;
    }
    return -1;
}

static bool session_id_present(const cp_link_sync_t *sync, uint8_t id)
{
    return id != 0u && session_type_for_id(sync, id) >= 0;
}

static void send_syn(cp_link_t *link, bool resend, uint64_t now_ms)
{
    uint8_t payload[10u + CP_LINK_MAX_SESSIONS * 3u];
    uint8_t frame[CP_LINK_MAX_FRAME];
    size_t payload_len;
    size_t frame_len;

    if (resend && (!link->syn_sent || link->syn_len == 0u)) {
        terminate_link(link, CP_LINK_RESET_INTERNAL, false);
        return;
    }

    /* Rebuild on every transmission. The sequence stays the initial SYN
     * sequence, but the acknowledgement field must reflect whatever was
     * received since the previous attempt, and the header checksum follows. */
    payload_len = sync_encode(&link->local_sync, payload, sizeof(payload));
    if (payload_len == 0) {
        terminate_link(link, CP_LINK_RESET_INTERNAL, false);
        return;
    }
    frame_len = build_frame(frame, sizeof(frame), CP_LINK_CONTROL_SYN,
                            link->sent_psn, link->last_received_psn, 0u, true,
                            payload, payload_len);
    if (frame_len == 0) {
        terminate_link(link, CP_LINK_RESET_INTERNAL, false);
        return;
    }
    memcpy(link->syn_frame, frame, frame_len);
    link->syn_len = (uint16_t)frame_len;
    link->syn_sent = true;

    if (!output_enqueue(link, frame, frame_len)) {
        terminate_link(link, CP_LINK_RESET_TX_BACKPRESSURE, false);
        return;
    }
    link->cumulative_received = 0;
    link->stats.syn_sent++;
    link->negotiate_deadline_ms =
        add_ms(now_ms, link->cfg.negotiate_interval_ms);
}

static void enter_negotiate(cp_link_t *link, uint64_t now_ms)
{
    if (link->state == CP_LINK_STATE_DETECT) {
        link->state = CP_LINK_STATE_NEGOTIATE;
        link->detect_deadline_ms = 0;
        link->detect_retries = 0;
        emit_state(link);
    }
    if (link->state != CP_LINK_STATE_NEGOTIATE)
        return;

    if (!link->syn_sent)
        send_syn(link, false, now_ms);
    else if (link->negotiate_deadline_ms == 0)
        link->negotiate_deadline_ms =
            add_ms(now_ms, link->cfg.negotiate_interval_ms);
}

static void deliver_session_data(cp_link_t *link, uint8_t session_id,
                                 const uint8_t *data, uint16_t len)
{
    cp_link_event_t event;
    int type;

    type = session_type_for_id(&link->peer_sync, session_id);
    if (type < 0) {
        /* A frame on an unnegotiated session is still sequenced/ACKed but
         * must not be surfaced as application data. */
        link->stats.rx_unknown_session++;
        return;
    }

    memset(&event, 0, sizeof(event));
    event.type = CP_LINK_EVENT_SESSION_DATA;
    event.state = link->state;
    event.generation = link->generation;
    event.session_id = session_id;
    event.session_type = (uint8_t)type;
    event.data = data;
    event.data_len = len;
    emit_event(link, &event);
    link->stats.data_received++;
}

static void after_data_ack(cp_link_t *link, uint64_t now_ms)
{
    uint16_t threshold;
    uint8_t distance;

    if (link->peer_sync.max_ack == 0)
        return;

    if (link->cumulative_received >= link->peer_sync.max_ack) {
        send_ack(link);
        return;
    }

    threshold = link->peer_sync.max_outgoing;
    distance = link->last_acked_valid
                   ? cp_link_sequence_distance(link->last_received_psn,
                                               link->last_acked_psn)
                   : 0u;
    if (threshold == 0 || distance >= (uint8_t)threshold) {
        send_ack(link);
        return;
    }

    link->delayed_ack_armed = true;
    link->delayed_ack_deadline_ms =
        add_ms(now_ms, link->peer_sync.ack_timeout_ms);
    if (link->peer_sync.ack_timeout_ms == 0) {
        link->delayed_ack_armed = false;
        send_ack(link);
    }
}

static void handle_data(cp_link_t *link, const cp_link_header_t *header,
                        const uint8_t *payload, uint16_t payload_len,
                        uint64_t now_ms)
{
    uint8_t distance;
    uint16_t window;
    uint8_t i;

    if (link->state != CP_LINK_STATE_NORMAL) {
        link->stats.data_ignored++;
        return;
    }

    distance = cp_link_sequence_distance(header->seq,
                                         link->last_received_psn);
    window = (uint16_t)link->peer_sync.max_outgoing + CP_LINK_RX_WINDOW_SLACK;
    if (window > 127u)
        window = 127u;

    if (distance == 0u) {
        link->stats.duplicates++;
        send_ack(link);
        return;
    }
    if ((uint16_t)distance > window || distance >= 128u) {
        link->stats.out_of_window++;
        send_ack(link);
        return;
    }

    if (distance > 1u) {
        cp_link_rx_slot_t *slot = NULL;

        for (i = 0; i < CP_LINK_MAX_REORDER; i++) {
            if (link->rx_slots[i].used &&
                link->rx_slots[i].psn == header->seq) {
                link->stats.duplicates++;
                send_ack(link);
                return;
            }
        }
        if (link->rx_count >= CP_LINK_MAX_REORDER) {
            link->stats.reorder_full++;
            send_ack(link);
            return;
        }
        for (i = 0; i < CP_LINK_MAX_REORDER; i++) {
            if (!link->rx_slots[i].used) {
                slot = &link->rx_slots[i];
                break;
            }
        }
        if (slot == NULL) {
            link->stats.reorder_full++;
            send_ack(link);
            return;
        }
        slot->used = true;
        slot->psn = header->seq;
        slot->session_id = header->session_id;
        slot->len = payload_len;
        if (payload_len != 0)
            memcpy(slot->data, payload, payload_len);
        link->rx_count++;
        link->stats.out_of_order++;

        if ((uint16_t)distance >= (uint16_t)link->peer_sync.max_outgoing)
            link->eak_due = true;
        eak_refresh(link);
        return;
    }

    deliver_session_data(link, header->session_id, payload, payload_len);
    link->last_received_psn = header->seq;

    for (;;) {
        cp_link_rx_slot_t *next = NULL;

        for (i = 0; i < CP_LINK_MAX_REORDER; i++) {
            if (link->rx_slots[i].used &&
                cp_link_sequence_distance(link->rx_slots[i].psn,
                                          link->last_received_psn) == 1u) {
                next = &link->rx_slots[i];
                break;
            }
        }
        if (next == NULL)
            break;
        next->used = false;
        link->rx_count--;
        link->last_received_psn = next->psn;
        deliver_session_data(link, next->session_id, next->data, next->len);
    }

    eak_refresh(link);
    after_data_ack(link, now_ms);
}

static void handle_eak(cp_link_t *link, const uint8_t *payload,
                       uint16_t payload_len, uint64_t now_ms)
{
    uint16_t i;

    if (link->state != CP_LINK_STATE_NORMAL)
        return;
    link->stats.eak_received++;

    /* Mark every requested PSN due, then let the shared retransmit pass
     * service them. If output is already full, the pass sets tx_blocked and
     * leaves the slots pending; the caller drains output and ticks again.
     * No retransmission is silently discarded. */
    for (i = 0; i < payload_len; i++) {
        uint8_t j;

        for (j = 0; j < CP_LINK_MAX_OUTSTANDING; j++) {
            cp_link_tx_slot_t *slot = &link->tx_slots[j];

            if (slot->used && slot->psn == payload[i]) {
                slot->deadline_ms = now_ms;
                break;
            }
        }
    }
    (void)retransmit_due(link, now_ms);
}

static void handle_ack(cp_link_t *link, const cp_link_header_t *header)
{
    uint8_t advance;
    uint8_t ahead;
    uint8_t i;

    if (link->state == CP_LINK_STATE_NEGOTIATE) {
        if (!link->peer_sync_valid || header->ack != link->sent_psn) {
            link->stats.ack_ignored++;
            return;
        }
        link->state = CP_LINK_STATE_NORMAL;
        link->negotiate_deadline_ms = 0;
        link->peer_ack = header->ack;
        link->peer_ack_valid = true;
        link->stats.ack_received++;
        emit_state(link);
        update_writable(link);
        return;
    }

    if (link->state != CP_LINK_STATE_NORMAL)
        return;
    if (!link->peer_ack_valid) {
        link->stats.ack_ignored++;
        return;
    }

    advance = cp_link_sequence_distance(header->ack, link->peer_ack);
    if (advance == 0u) {
        link->stats.ack_ignored++;
        return;
    }
    if (advance >= 128u) {
        link->stats.ack_ignored++;
        return;
    }
    ahead = cp_link_sequence_distance(header->ack, link->sent_psn);
    if (ahead != 0u && ahead < 128u) {
        link->stats.ack_ignored++;
        return;
    }

    for (i = 0; i < CP_LINK_MAX_OUTSTANDING; i++) {
        cp_link_tx_slot_t *slot = &link->tx_slots[i];

        if (!slot->used)
            continue;
        if (cp_link_sequence_distance(header->ack, slot->psn) < 128u) {
            slot->used = false;
            if (link->tx_count > 0)
                link->tx_count--;
        }
    }

    link->peer_ack = header->ack;
    link->stats.ack_received++;
    update_writable(link);
}

static void handle_syn(cp_link_t *link, const cp_link_header_t *header,
                       const uint8_t *payload, uint16_t payload_len,
                       uint64_t now_ms)
{
    cp_link_sync_t parsed;

    if (link->state != CP_LINK_STATE_DETECT &&
        link->state != CP_LINK_STATE_NEGOTIATE) {
        link->stats.syn_ignored++;
        return;
    }
    if (payload_len >
        10u + 3u * (size_t)CP_LINK_MAX_SESSIONS) {
        link->stats.syn_invalid++;
        terminate_link(link, CP_LINK_RESET_LIMITS, false);
        return;
    }
    if (!sync_parse(payload, payload_len, &parsed)) {
        link->stats.syn_invalid++;
        return;
    }
    if (parsed.max_outgoing > CP_LINK_MAX_OUTSTANDING) {
        terminate_link(link, CP_LINK_RESET_LIMITS, false);
        return;
    }
    if (session_id_for_type(&parsed, CP_LINK_SESSION_CONTROL) < 0) {
        terminate_link(link, CP_LINK_RESET_SESSION_UNSUPPORTED, false);
        return;
    }

    link->peer_sync = parsed;
    link->peer_sync_valid = true;
    link->stats.syn_received++;

    enter_negotiate(link, now_ms);
    if (link->state == CP_LINK_STATE_DEAD)
        return;

    link->last_received_psn = header->seq;
    link->last_acked_psn = header->seq;
    link->last_acked_valid = true;
    link->delayed_ack_armed = false;
    link->delayed_ack_deadline_ms = 0;
    link->cumulative_received = 0;
    send_ack(link);
}

static void process_frame(cp_link_t *link, const cp_link_header_t *header,
                          const uint8_t *payload, uint16_t payload_len,
                          uint64_t now_ms)
{
    link->stats.frames_valid++;

    if ((header->control & CP_LINK_CONTROL_RST) != 0u) {
        terminate_link(link, CP_LINK_RESET_PEER, false);
        return;
    }

    if ((header->control & CP_LINK_CONTROL_SYN) != 0u) {
        if (payload == NULL || payload_len == 0) {
            link->stats.syn_invalid++;
        } else {
            handle_syn(link, header, payload, payload_len, now_ms);
            if (link->state == CP_LINK_STATE_DEAD)
                return;
        }
    }

    if ((header->control & CP_LINK_CONTROL_ACK) != 0u) {
        if (link->cumulative_received != UINT16_MAX)
            link->cumulative_received++;
        handle_ack(link, header);
        if (link->state == CP_LINK_STATE_DEAD)
            return;
    }

    if ((header->control & CP_LINK_CONTROL_EAK) != 0u) {
        if (payload != NULL && payload_len != 0) {
            handle_eak(link, payload, payload_len, now_ms);
            if (link->state == CP_LINK_STATE_DEAD)
                return;
        }
    }

    if ((uint8_t)(header->control & (uint8_t)~CP_LINK_CONTROL_ACK) == 0u &&
        payload != NULL) {
        handle_data(link, header, payload, payload_len, now_ms);
    } else if (payload != NULL &&
               (header->control &
                (CP_LINK_CONTROL_SYN | CP_LINK_CONTROL_EAK)) == 0u) {
        link->stats.data_ignored++;
    }

    if (link->state == CP_LINK_STATE_NORMAL && link->peer_sync_valid &&
        link->peer_sync.max_ack > 0 &&
        link->cumulative_received >= link->peer_sync.max_ack)
        send_ack(link);
}

static void handle_marker(cp_link_t *link, uint64_t now_ms)
{
    if (link->rx_phase != CP_LINK_RX_MARKER)
        return;

    link->stats.marker_received++;
    link->rx_phase = CP_LINK_RX_HEADER;
    link->header_len = 0;
    enter_negotiate(link, now_ms);
}

static void build_local_sync(cp_link_t *link)
{
    cp_link_sync_t *sync = &link->local_sync;
    bool zero = link->cfg.zero_ack;

    memset(sync, 0, sizeof(*sync));
    sync->version = CP_LINK_SYNC_VERSION;
    sync->max_outgoing = link->cfg.max_outgoing;
    sync->max_len = (uint16_t)(link->cfg.max_payload + 10u);
    sync->retransmission_timeout_ms =
        zero ? 0u : link->cfg.retransmission_timeout_ms;
    sync->ack_timeout_ms = zero ? 0u : link->cfg.ack_timeout_ms;
    sync->max_retransmissions = zero ? 0u : link->cfg.max_retransmissions;
    sync->max_ack = zero ? 0u : link->cfg.max_ack;

    sync->sessions[sync->session_count].id = link->cfg.control_session_id;
    sync->sessions[sync->session_count].type = CP_LINK_SESSION_CONTROL;
    sync->sessions[sync->session_count].version = link->cfg.control_version;
    sync->session_count++;

    if (link->cfg.external_accessory_session_id != 0u) {
        sync->sessions[sync->session_count].id =
            link->cfg.external_accessory_session_id;
        sync->sessions[sync->session_count].type =
            CP_LINK_SESSION_EXTERNAL_ACCESSORY;
        sync->sessions[sync->session_count].version = 1u;
        sync->session_count++;
    }
    if (link->cfg.file_transfer_session_id != 0u) {
        sync->sessions[sync->session_count].id =
            link->cfg.file_transfer_session_id;
        sync->sessions[sync->session_count].type =
            CP_LINK_SESSION_FILE_TRANSFER;
        sync->sessions[sync->session_count].version = 2u;
        sync->session_count++;
    }
}

static bool config_valid(const cp_link_config_t *cfg)
{
    if (cfg == NULL)
        return false;
    if (cfg->control_session_id == 0u)
        return false;
    if (cfg->max_payload == 0u || cfg->max_payload > CP_LINK_MAX_PAYLOAD)
        return false;
    if (cfg->max_outgoing == 0u ||
        cfg->max_outgoing > CP_LINK_MAX_OUTSTANDING)
        return false;
    if (cfg->detect_interval_ms == 0u ||
        cfg->negotiate_interval_ms == 0u ||
        cfg->handshake_timeout_ms == 0u)
        return false;
    if (cfg->detect_retry_limit > CP_LINK_MAX_RETRY_LIMIT ||
        cfg->negotiate_retry_limit > CP_LINK_MAX_RETRY_LIMIT)
        return false;
    if (cfg->max_retransmissions > 0u &&
        cfg->retransmission_timeout_ms == 0u)
        return false;
    if (cfg->external_accessory_session_id != 0u &&
        cfg->external_accessory_session_id == cfg->control_session_id)
        return false;
    if (cfg->file_transfer_session_id != 0u &&
        (cfg->file_transfer_session_id == cfg->control_session_id ||
         cfg->file_transfer_session_id ==
             cfg->external_accessory_session_id))
        return false;
    return true;
}

void cp_link_config_default(cp_link_config_t *cfg)
{
    if (cfg == NULL)
        return;

    /* Conservative local policy, not a live-validated profile. The observed
     * live Bluetooth profile is cp_link_config_live_bluetooth(). */
    memset(cfg, 0, sizeof(*cfg));
    cfg->initial_seq = CP_LINK_DEFAULT_INITIAL_SEQ;
    cfg->control_session_id = 10u;
    cfg->external_accessory_session_id = 11u;
    cfg->file_transfer_session_id = 12u;
    cfg->control_version = 1u;
    cfg->max_outgoing = CP_LINK_MAX_OUTSTANDING < 30u
                            ? (uint8_t)CP_LINK_MAX_OUTSTANDING
                            : 30u;
    cfg->max_payload = CP_LINK_MAX_PAYLOAD < 4096u
                           ? (uint16_t)CP_LINK_MAX_PAYLOAD
                           : 4096u;
    cfg->retransmission_timeout_ms = 4000u;
    cfg->ack_timeout_ms = 500u;
    cfg->max_retransmissions = 4u;
    cfg->max_ack = 3u;
    cfg->zero_ack = false;
    cfg->detect_interval_ms = 1000u;
    cfg->detect_retry_limit = 8u;
    cfg->negotiate_interval_ms = 500u;
    cfg->negotiate_retry_limit = 8u;
    cfg->handshake_timeout_ms = 15000u;
}

void cp_link_config_live_bluetooth(cp_link_config_t *cfg)
{
    if (cfg == NULL)
        return;

    cp_link_config_default(cfg);
    cfg->control_version = 2u;
    cfg->max_outgoing = 4u;
    cfg->zero_ack = false;
}

int cp_link_init(cp_link_t *link, const cp_link_config_t *cfg,
                 cp_link_event_fn on_event, void *user)
{
    cp_link_config_t defaults;

    if (link == NULL)
        return CP_LINK_ERR_INVALID;
    memset(link, 0, sizeof(*link));
    if (cfg == NULL) {
        cp_link_config_default(&defaults);
        cfg = &defaults;
    }
    if (on_event == NULL || !config_valid(cfg))
        return CP_LINK_ERR_INVALID;

    link->cfg = *cfg;
    link->initial_seq = cfg->initial_seq;
    link->on_event = on_event;
    link->user = user;
    link->state = CP_LINK_STATE_IDLE;
    link->initialized = true;
    build_local_sync(link);
    return CP_LINK_OK;
}

int cp_link_start(cp_link_t *link, cp_link_role_t role, uint64_t now_ms)
{
    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (role != CP_LINK_ROLE_PASSIVE && role != CP_LINK_ROLE_INITIATOR)
        return CP_LINK_ERR_INVALID;
    if (link->state == CP_LINK_STATE_DETECT ||
        link->state == CP_LINK_STATE_NEGOTIATE ||
        link->state == CP_LINK_STATE_NORMAL)
        return CP_LINK_ERR_STATE;

    clear_output(link);
    clear_tx(link);
    clear_rx(link);
    clear_peer_sync(link);
    clear_deadlines(link);
    clear_parser(link);
    memset(&link->stats, 0, sizeof(link->stats));

    link->state = CP_LINK_STATE_DETECT;
    link->reset_reason = CP_LINK_RESET_NONE;
    link->generation++;
    link->now_ms = now_ms;
    link->sent_psn = link->initial_seq;
    link->peer_ack = 0u;
    link->peer_ack_valid = false;
    link->last_received_psn = 0u;
    link->last_acked_psn = 0u;
    link->last_acked_valid = false;
    link->syn_sent = false;
    link->syn_len = 0;
    link->detect_retries = 0;
    link->syn_retries = 0;
    link->cumulative_received = 0;
    link->delayed_ack_armed = false;
    link->writable = false;
    link->tx_blocked = false;
    link->rx_phase = role == CP_LINK_ROLE_PASSIVE ? CP_LINK_RX_MARKER
                                                  : CP_LINK_RX_HEADER;
    link->handshake_deadline_ms =
        add_ms(now_ms, link->cfg.handshake_timeout_ms);

    emit_state(link);
    send_marker(link, now_ms);
    if (role == CP_LINK_ROLE_INITIATOR)
        send_syn(link, false, now_ms);
    return CP_LINK_OK;
}

int cp_link_feed(cp_link_t *link, const void *data, size_t len,
                 uint64_t now_ms)
{
    const uint8_t *input = (const uint8_t *)data;
    size_t left;

    if (link == NULL || !link->initialized ||
        (data == NULL && len != 0))
        return CP_LINK_ERR_INVALID;
    if (link->state == CP_LINK_STATE_IDLE ||
        link->state == CP_LINK_STATE_DEAD)
        return CP_LINK_ERR_STATE;

    link->now_ms = now_ms;
    if (len > 0xFFFFFFFFu)
        link->stats.bytes_fed += 0xFFFFFFFFu;
    else
        link->stats.bytes_fed += (uint32_t)len;

    left = len;
    while (left > 0u && link->state != CP_LINK_STATE_DEAD) {
        if (link->rx_phase == CP_LINK_RX_MARKER) {
            size_t need = CP_LINK_MARKER_LEN - link->marker_len;
            size_t take = left < need ? left : need;

            memcpy(link->marker_buf + link->marker_len, input, take);
            link->marker_len = (uint8_t)(link->marker_len + (uint8_t)take);
            input += take;
            left -= take;
            if (link->marker_len < CP_LINK_MARKER_LEN)
                break;
            if (memcmp(link->marker_buf, cp_link_marker,
                       CP_LINK_MARKER_LEN) != 0) {
                terminate_link(link, CP_LINK_RESET_MARKER, false);
                break;
            }
            link->marker_len = 0;
            handle_marker(link, now_ms);
            continue;
        }

        if (link->rx_phase == CP_LINK_RX_HEADER) {
            size_t need = CP_LINK_HEADER_LEN - link->header_len;
            size_t take = left < need ? left : need;

            memcpy(link->header_buf + link->header_len, input, take);
            link->header_len =
                (uint8_t)(link->header_len + (uint8_t)take);
            input += take;
            left -= take;
            if (link->header_len < CP_LINK_HEADER_LEN)
                break;

            if (link->header_buf[0] != 0xFFu ||
                link->header_buf[1] != 0x5Au) {
                memmove(link->header_buf, link->header_buf + 1,
                        CP_LINK_HEADER_LEN - 1u);
                link->header_len = CP_LINK_HEADER_LEN - 1u;
                link->stats.resync_bytes++;
                continue;
            }
            if (sum8(link->header_buf, CP_LINK_HEADER_LEN) != 0u) {
                link->stats.frames_bad_header++;
                memmove(link->header_buf, link->header_buf + 1,
                        CP_LINK_HEADER_LEN - 1u);
                link->header_len = CP_LINK_HEADER_LEN - 1u;
                link->stats.resync_bytes++;
                continue;
            }

            link->current_header.length = get_be16(link->header_buf + 2);
            link->current_header.control = link->header_buf[4];
            link->current_header.seq = link->header_buf[5];
            link->current_header.ack = link->header_buf[6];
            link->current_header.session_id = link->header_buf[7];
            link->header_len = 0;

            if (link->current_header.length < CP_LINK_HEADER_LEN ||
                link->current_header.length >
                    (uint16_t)(link->cfg.max_payload + 10u)) {
                link->stats.frames_bad_length++;
                link->header_len = CP_LINK_HEADER_LEN - 1u;
                memmove(link->header_buf, link->header_buf + 1,
                        CP_LINK_HEADER_LEN - 1u);
                link->stats.resync_bytes++;
                continue;
            }
            if (link->current_header.length == CP_LINK_HEADER_LEN) {
                process_frame(link, &link->current_header, NULL, 0u, now_ms);
                continue;
            }
            link->payload_need =
                (uint16_t)(link->current_header.length - CP_LINK_HEADER_LEN);
            link->payload_len = 0;
            link->rx_phase = CP_LINK_RX_PAYLOAD;
            continue;
        }

        if (link->rx_phase == CP_LINK_RX_PAYLOAD) {
            size_t need = (size_t)link->payload_need -
                          (size_t)link->payload_len;
            size_t take = left < need ? left : need;

            memcpy(link->payload_buf + link->payload_len, input, take);
            link->payload_len =
                (uint16_t)(link->payload_len + (uint16_t)take);
            input += take;
            left -= take;
            if (link->payload_len < link->payload_need)
                break;

            link->rx_phase = CP_LINK_RX_HEADER;
            if (sum8(link->payload_buf, link->payload_need) != 0u) {
                link->stats.frames_bad_payload++;
                continue;
            }
            process_frame(link, &link->current_header, link->payload_buf,
                          (uint16_t)(link->payload_need - 1u), now_ms);
            continue;
        }
    }

    return CP_LINK_OK;
}

static bool retransmit_due(cp_link_t *link, uint64_t now_ms)
{
    bool did = false;
    uint8_t i;

    if (link->tx_blocked)
        return false;

    for (i = 0; i < CP_LINK_MAX_OUTSTANDING; i++) {
        cp_link_tx_slot_t *slot = &link->tx_slots[i];

        if (!slot->used || now_ms < slot->deadline_ms)
            continue;
        if (link->peer_sync.max_retransmissions == 0) {
            slot->used = false;
            if (link->tx_count > 0)
                link->tx_count--;
            continue;
        }
        if (link->out_count >= CP_LINK_OUTPUT_FRAMES) {
            link->tx_blocked = true;
            update_writable(link);
            return did;
        }
        slot->retries++;
        if (slot->retries >= link->peer_sync.max_retransmissions) {
            terminate_link(link, CP_LINK_RESET_ACK_TIMEOUT, false);
            return true;
        }
        {
            /* Keep sequence, session and payload; refresh only the cumulative
             * ACK and header checksum in an enqueued copy. The stored original
             * frame stays untouched. */
            uint8_t frame[CP_LINK_MAX_FRAME];

            memcpy(frame, slot->frame, slot->len);
            frame[6] = link->last_received_psn;
            frame[8] = checksum8(frame, CP_LINK_HEADER_LEN - 1u);
            if (!output_enqueue(link, frame, slot->len)) {
                link->tx_blocked = true;
                update_writable(link);
                return did;
            }
        }
        slot->deadline_ms =
            add_ms(now_ms, link->peer_sync.retransmission_timeout_ms);
        link->stats.retransmits++;
        link->delayed_ack_armed = false;
        link->delayed_ack_deadline_ms = 0;
        did = true;
    }
    return did;
}

int cp_link_tick(cp_link_t *link, uint64_t now_ms)
{
    unsigned iteration;

    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (link->state == CP_LINK_STATE_IDLE)
        return CP_LINK_ERR_STATE;
    if (link->state == CP_LINK_STATE_DEAD)
        return CP_LINK_OK;

    link->now_ms = now_ms;
    for (iteration = 0; iteration < 64u; iteration++) {
        bool did = false;

        if ((link->state == CP_LINK_STATE_DETECT ||
             link->state == CP_LINK_STATE_NEGOTIATE) &&
            link->handshake_deadline_ms != 0 &&
            now_ms >= link->handshake_deadline_ms) {
            terminate_link(link, CP_LINK_RESET_HANDSHAKE_TIMEOUT, false);
            return CP_LINK_OK;
        }

        if (link->state == CP_LINK_STATE_DETECT &&
            link->detect_deadline_ms != 0 &&
            now_ms >= link->detect_deadline_ms) {
            if (link->detect_retries < link->cfg.detect_retry_limit) {
                link->detect_retries++;
                send_marker(link, now_ms);
            } else {
                terminate_link(link, CP_LINK_RESET_DETECT_EXHAUSTED, false);
                return CP_LINK_OK;
            }
            did = true;
            if (link->state == CP_LINK_STATE_DEAD)
                return CP_LINK_OK;
        }

        if (link->state == CP_LINK_STATE_NEGOTIATE &&
            link->negotiate_deadline_ms != 0 &&
            now_ms >= link->negotiate_deadline_ms) {
            if (link->syn_retries < link->cfg.negotiate_retry_limit) {
                link->syn_retries++;
                send_syn(link, true, now_ms);
            } else {
                terminate_link(link, CP_LINK_RESET_NEGOTIATE_EXHAUSTED,
                               false);
                return CP_LINK_OK;
            }
            did = true;
            if (link->state == CP_LINK_STATE_DEAD)
                return CP_LINK_OK;
        }

        if (link->delayed_ack_armed &&
            link->delayed_ack_deadline_ms != 0 &&
            now_ms >= link->delayed_ack_deadline_ms) {
            link->delayed_ack_armed = false;
            link->delayed_ack_deadline_ms = 0;
            if (link->state == CP_LINK_STATE_NORMAL)
                send_ack(link);
            did = true;
            if (link->state == CP_LINK_STATE_DEAD)
                return CP_LINK_OK;
        }

        if (link->eak_due) {
            eak_refresh(link);
            if (!link->eak_due)
                did = true;
        }

        if (link->ack_due) {
            send_ack(link);
            if (!link->ack_due)
                did = true;
        }

        if (link->state == CP_LINK_STATE_NORMAL && !link->tx_blocked &&
            retransmit_due(link, now_ms)) {
            did = true;
            if (link->state == CP_LINK_STATE_DEAD)
                return CP_LINK_OK;
        }

        if (!did)
            break;
    }
    return CP_LINK_OK;
}

size_t cp_link_output_peek(const cp_link_t *link, const uint8_t **data)
{
    const cp_link_out_slot_t *slot;

    if (data != NULL)
        *data = NULL;
    if (link == NULL || link->out_count == 0)
        return 0;
    slot = &link->out_slots[link->out_head];
    if (slot->consumed >= slot->len)
        return 0;
    if (data != NULL)
        *data = slot->bytes + slot->consumed;
    return (size_t)(slot->len - slot->consumed);
}

size_t cp_link_output_pending(const cp_link_t *link)
{
    size_t total = 0;
    unsigned i;

    if (link == NULL)
        return 0;
    for (i = 0; i < link->out_count; i++) {
        unsigned index = ((unsigned)link->out_head + i) %
                         (unsigned)CP_LINK_OUTPUT_FRAMES;
        const cp_link_out_slot_t *slot = &link->out_slots[index];

        total += (size_t)(slot->len - slot->consumed);
    }
    return total;
}

int cp_link_output_consume(cp_link_t *link, size_t len)
{
    cp_link_out_slot_t *slot;
    size_t remaining;

    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (len == 0)
        return CP_LINK_OK;
    if (link->out_count == 0)
        return CP_LINK_ERR_INVALID;

    slot = &link->out_slots[link->out_head];
    remaining = (size_t)(slot->len - slot->consumed);
    if (len > remaining)
        return CP_LINK_ERR_INVALID;

    slot->consumed = (uint16_t)(slot->consumed + len);
    if (slot->consumed == slot->len) {
        slot->consumed = 0;
        link->out_head =
            (uint8_t)(((unsigned)link->out_head + 1u) %
                      (unsigned)CP_LINK_OUTPUT_FRAMES);
        link->out_count--;
        link->tx_blocked = false;
        if (link->eak_due)
            eak_refresh(link);
        if (link->ack_due)
            send_ack(link);
        update_writable(link);
    }
    return CP_LINK_OK;
}

uint64_t cp_link_next_deadline(const cp_link_t *link)
{
    uint64_t deadline = UINT64_MAX;
    uint8_t i;

    if (link == NULL || !link->initialized ||
        link->state == CP_LINK_STATE_IDLE ||
        link->state == CP_LINK_STATE_DEAD)
        return UINT64_MAX;

    if (link->state == CP_LINK_STATE_DETECT ||
        link->state == CP_LINK_STATE_NEGOTIATE) {
        if (link->handshake_deadline_ms != 0)
            deadline = deadline_min(deadline, link->handshake_deadline_ms);
        if (link->detect_deadline_ms != 0)
            deadline = deadline_min(deadline, link->detect_deadline_ms);
        if (link->negotiate_deadline_ms != 0)
            deadline = deadline_min(deadline, link->negotiate_deadline_ms);
    }
    if (link->state == CP_LINK_STATE_NORMAL) {
        if (link->delayed_ack_armed &&
            link->delayed_ack_deadline_ms != 0)
            deadline = deadline_min(deadline, link->delayed_ack_deadline_ms);
        if (!link->tx_blocked) {
            for (i = 0; i < CP_LINK_MAX_OUTSTANDING; i++) {
                if (link->tx_slots[i].used)
                    deadline =
                        deadline_min(deadline, link->tx_slots[i].deadline_ms);
            }
        }
    }
    return deadline;
}

int cp_link_send_session(cp_link_t *link, uint8_t session_id,
                         const void *data, size_t len, uint64_t now_ms)
{
    uint8_t frame[CP_LINK_MAX_FRAME];
    uint8_t new_psn;
    size_t frame_len;
    size_t max_payload;
    cp_link_tx_slot_t *slot = NULL;
    uint8_t i;

    if (link == NULL || !link->initialized ||
        (data == NULL && len != 0))
        return CP_LINK_ERR_INVALID;
    if (link->state != CP_LINK_STATE_NORMAL)
        return CP_LINK_ERR_STATE;
    if (!link->peer_sync_valid ||
        !session_id_present(&link->peer_sync, session_id))
        return CP_LINK_ERR_NO_SESSION;
    if (len > CP_LINK_MAX_PAYLOAD)
        return CP_LINK_ERR_TOO_LARGE;

    max_payload = cp_link_max_payload(link);
    if (len > max_payload)
        return CP_LINK_ERR_TOO_LARGE;

    if (link->tx_blocked ||
        (unsigned)link->out_count + CP_LINK_OUTPUT_RESERVED >=
            (unsigned)CP_LINK_OUTPUT_FRAMES) {
        link->stats.tx_backpressure++;
        return CP_LINK_ERR_BACKPRESSURE;
    }
    if (link->peer_sync.max_retransmissions > 0) {
        if (link->peer_sync.max_outgoing == 0 ||
            link->tx_count >= link->peer_sync.max_outgoing) {
            link->stats.tx_backpressure++;
            return CP_LINK_ERR_BACKPRESSURE;
        }
        for (i = 0; i < CP_LINK_MAX_OUTSTANDING; i++) {
            if (!link->tx_slots[i].used) {
                slot = &link->tx_slots[i];
                break;
            }
        }
        if (slot == NULL) {
            link->stats.tx_backpressure++;
            return CP_LINK_ERR_BACKPRESSURE;
        }
    }

    new_psn = (uint8_t)(link->sent_psn + 1u);
    frame_len = build_frame(frame, sizeof(frame), CP_LINK_CONTROL_ACK,
                            new_psn, link->last_received_psn, session_id,
                            true, (const uint8_t *)data, len);
    if (frame_len == 0)
        return CP_LINK_ERR_INVALID;
    if ((uint16_t)frame_len > link->peer_sync.max_len)
        return CP_LINK_ERR_TOO_LARGE;
    if (!output_enqueue(link, frame, frame_len)) {
        link->stats.tx_backpressure++;
        return CP_LINK_ERR_BACKPRESSURE;
    }

    link->sent_psn = new_psn;
    link->cumulative_received = 0;
    if (slot != NULL) {
        slot->used = true;
        slot->psn = new_psn;
        slot->session_id = session_id;
        slot->retries = 0;
        slot->deadline_ms =
            add_ms(now_ms, link->peer_sync.retransmission_timeout_ms);
        slot->len = (uint16_t)frame_len;
        memcpy(slot->frame, frame, frame_len);
        link->tx_count++;
    }

    link->stats.data_sent++;
    link->delayed_ack_armed = false;
    link->delayed_ack_deadline_ms = 0;
    link->last_acked_psn = link->last_received_psn;
    link->last_acked_valid = true;
    update_writable(link);
    return CP_LINK_OK;
}

int cp_link_send(cp_link_t *link, uint8_t session_kind, const void *data,
                 size_t len, uint64_t now_ms)
{
    int session_id;

    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (link->state != CP_LINK_STATE_NORMAL)
        return CP_LINK_ERR_STATE;
    session_id = cp_link_session_id(link, session_kind);
    if (session_id < 0)
        return CP_LINK_ERR_NO_SESSION;
    return cp_link_send_session(link, (uint8_t)session_id, data, len, now_ms);
}

int cp_link_cancel(cp_link_t *link, cp_link_reset_reason_t reason)
{
    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (link->state == CP_LINK_STATE_DEAD ||
        link->state == CP_LINK_STATE_IDLE)
        return CP_LINK_OK;
    terminate_link(link,
                   reason == CP_LINK_RESET_NONE ? CP_LINK_RESET_CANCELLED
                                                : reason,
                   false);
    return CP_LINK_OK;
}

int cp_link_abort(cp_link_t *link, uint64_t now_ms,
                  cp_link_reset_reason_t reason)
{
    cp_link_reset_reason_t selected;
    uint8_t frame[CP_LINK_HEADER_LEN];
    size_t frame_len;

    if (link == NULL || !link->initialized)
        return CP_LINK_ERR_INVALID;
    if (link->state == CP_LINK_STATE_IDLE)
        return CP_LINK_ERR_STATE;
    if (link->state == CP_LINK_STATE_DEAD)
        return CP_LINK_OK;

    link->now_ms = now_ms;
    clear_output(link);
    frame_len = build_frame(frame, sizeof(frame), CP_LINK_CONTROL_RST,
                            link->sent_psn, link->last_received_psn, 0u,
                            false, NULL, 0u);
    selected = reason == CP_LINK_RESET_NONE ? CP_LINK_RESET_LOCAL_ABORT
                                            : reason;
    if (frame_len == 0 || !output_enqueue(link, frame, frame_len)) {
        terminate_link(link, CP_LINK_RESET_TX_BACKPRESSURE, false);
        return CP_LINK_OK;
    }
    terminate_link(link, selected, true);
    return CP_LINK_OK;
}

cp_link_state_t cp_link_state(const cp_link_t *link)
{
    return link == NULL ? CP_LINK_STATE_IDLE : link->state;
}

bool cp_link_writable(const cp_link_t *link)
{
    return link != NULL && link->writable;
}

uint32_t cp_link_generation(const cp_link_t *link)
{
    return link == NULL ? 0u : link->generation;
}

cp_link_reset_reason_t cp_link_reset_reason(const cp_link_t *link)
{
    return link == NULL ? CP_LINK_RESET_NONE : link->reset_reason;
}

const char *cp_link_reset_reason_string(cp_link_reset_reason_t reason)
{
    switch (reason) {
    case CP_LINK_RESET_NONE:
        return "none";
    case CP_LINK_RESET_CANCELLED:
        return "cancelled";
    case CP_LINK_RESET_LOCAL_ABORT:
        return "local-abort";
    case CP_LINK_RESET_MARKER:
        return "marker-mismatch";
    case CP_LINK_RESET_PEER:
        return "peer-reset";
    case CP_LINK_RESET_HANDSHAKE_TIMEOUT:
        return "handshake-timeout";
    case CP_LINK_RESET_DETECT_EXHAUSTED:
        return "detect-retries-exhausted";
    case CP_LINK_RESET_NEGOTIATE_EXHAUSTED:
        return "negotiate-retries-exhausted";
    case CP_LINK_RESET_ACK_TIMEOUT:
        return "ack-timeout";
    case CP_LINK_RESET_SYNC_INVALID:
        return "syn-invalid";
    case CP_LINK_RESET_SESSION_UNSUPPORTED:
        return "session-unsupported";
    case CP_LINK_RESET_LIMITS:
        return "limits-unsupported";
    case CP_LINK_RESET_TX_BACKPRESSURE:
        return "tx-backpressure";
    case CP_LINK_RESET_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}

int cp_link_session_id(const cp_link_t *link, uint8_t session_kind)
{
    if (link == NULL || !link->peer_sync_valid)
        return CP_LINK_SESSION_ID_NONE;
    if (session_kind != CP_LINK_SESSION_CONTROL &&
        session_kind != CP_LINK_SESSION_EXTERNAL_ACCESSORY &&
        session_kind != CP_LINK_SESSION_FILE_TRANSFER)
        return CP_LINK_SESSION_ID_NONE;
    return session_id_for_type(&link->peer_sync, session_kind);
}

const cp_link_sync_t *cp_link_local_sync(const cp_link_t *link)
{
    return link == NULL ? NULL : &link->local_sync;
}

const cp_link_sync_t *cp_link_peer_sync(const cp_link_t *link)
{
    if (link == NULL || !link->peer_sync_valid)
        return NULL;
    return &link->peer_sync;
}

size_t cp_link_max_payload(const cp_link_t *link)
{
    size_t local;
    size_t peer;

    if (link == NULL || !link->peer_sync_valid)
        return 0;
    local = link->cfg.max_payload;
    if (local > CP_LINK_MAX_PAYLOAD)
        local = CP_LINK_MAX_PAYLOAD;
    if (link->peer_sync.max_len <= CP_LINK_HEADER_LEN)
        return 0;
    peer = (size_t)link->peer_sync.max_len - CP_LINK_HEADER_LEN - 1u;
    return local < peer ? local : peer;
}

const cp_link_stats_t *cp_link_stats(const cp_link_t *link)
{
    return link == NULL ? NULL : &link->stats;
}
