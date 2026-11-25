/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Serialized cp_link + cp_bootstrap + cp_wire integration. See cp_session.h.
 * No allocation, sockets, hardware, clocks or logging.
 */
#include "cp_session.h"

#include <string.h>

#define CP_SESSION_CSM_START 0x4040u
#define CP_SESSION_CSM_HEADER_LEN 6u

_Static_assert(CP_SESSION_TX_QUEUE >= 1u, "reply queue slots");
_Static_assert(CP_SESSION_TX_QUEUE <= 255u, "queue count is u8");
_Static_assert(CP_SESSION_RX_BUFFER >= CP_WIRE_MAX_FRAME,
               "at least one maximum CSM");

static void refresh_link_stats(cp_session_t *session);

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static uint64_t add_ms(uint64_t now_ms, uint32_t delta_ms)
{
    if (UINT64_MAX - now_ms < (uint64_t)delta_ms)
        return UINT64_MAX;
    return now_ms + (uint64_t)delta_ms;
}

static const char *session_state_name_internal(cp_session_state_t state)
{
    switch (state) {
    case CP_SESSION_STATE_IDLE:
        return "idle";
    case CP_SESSION_STATE_STARTING:
        return "starting";
    case CP_SESSION_STATE_IDENTIFYING:
        return "identifying";
    case CP_SESSION_STATE_IDENTIFIED:
        return "identified";
    case CP_SESSION_STATE_AUTHENTICATING:
        return "authenticating";
    case CP_SESSION_STATE_AUTHENTICATED:
        return "authenticated";
    case CP_SESSION_STATE_AWAITING_RECEIVER:
        return "awaiting-receiver";
    case CP_SESSION_STATE_DISCONNECTED:
        return "disconnected";
    case CP_SESSION_STATE_FAILED:
        return "failed";
    case CP_SESSION_STATE_CANCELLED:
        return "cancelled";
    default:
        return "unknown";
    }
}

const char *cp_session_state_name(cp_session_state_t state)
{
    return session_state_name_internal(state);
}

const char *cp_session_error_name(cp_session_error_t error)
{
    switch (error) {
    case CP_SESSION_OK:
        return "ok";
    case CP_SESSION_ERR_ARGUMENT:
        return "argument";
    case CP_SESSION_ERR_CONFIG:
        return "config";
    case CP_SESSION_ERR_STATE:
        return "state";
    case CP_SESSION_ERR_CLOCK_ROLLBACK:
        return "clock-rollback";
    case CP_SESSION_ERR_LINK:
        return "link";
    case CP_SESSION_ERR_BOOTSTRAP:
        return "bootstrap";
    case CP_SESSION_ERR_CSM_MALFORMED:
        return "csm-malformed";
    case CP_SESSION_ERR_RX_OVERFLOW:
        return "rx-overflow";
    case CP_SESSION_ERR_TX_QUEUE_OVERFLOW:
        return "tx-queue-overflow";
    case CP_SESSION_ERR_TX_STALE:
        return "tx-stale";
    case CP_SESSION_ERR_LINK_LIMIT:
        return "link-limit";
    case CP_SESSION_ERR_AUTH_FAILED:
        return "auth-failed";
    case CP_SESSION_ERR_IDENTIFICATION_REJECTED:
        return "identification-rejected";
    case CP_SESSION_ERR_POLICY:
        return "policy";
    case CP_SESSION_ERR_TIMEOUT:
        return "timeout";
    case CP_SESSION_ERR_CANCELLED:
        return "cancelled";
    case CP_SESSION_ERR_GENERATION_MISMATCH:
        return "generation-mismatch";
    case CP_SESSION_ERR_PEER_MISMATCH:
        return "peer-mismatch";
    case CP_SESSION_ERR_RECEIVER_STATE:
        return "receiver-state";
    case CP_SESSION_ERR_PROVIDER:
        return "provider";
    default:
        return "unknown";
    }
}

static void emit_event(cp_session_t *session, cp_session_event_type_t type,
                       cp_session_error_t error, cp_boot_event boot_event,
                       cp_boot_error boot_error,
                       cp_link_reset_reason_t link_reset_reason)
{
    cp_session_event_t event;

    if (session->on_event == NULL)
        return;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.state = session->state;
    event.link_state = cp_link_state(&session->link);
    event.boot_state = cp_bootstrap_state(&session->boot);
    event.auth_state = cp_bootstrap_auth_state(&session->boot);
    event.ap_state = cp_bootstrap_ap_state(&session->boot);
    event.boot_event = boot_event;
    event.boot_error = boot_error;
    event.error = error;
    event.link_reset_reason = link_reset_reason;
    event.generation = cp_link_generation(&session->link);
    event.receiver_active = session->receiver_active;
    event.start_sent =
        cp_bootstrap_state(&session->boot) == CP_BOOT_STATE_AWAITING_RECEIVER;
    session->on_event(session->user, &event);
}

static void emit_state(cp_session_t *session)
{
    emit_event(session, CP_SESSION_EVENT_STATE, CP_SESSION_OK, CP_BOOT_EV_NONE,
               CP_BOOT_OK, CP_LINK_RESET_NONE);
}

static void set_state(cp_session_t *session, cp_session_state_t state)
{
    if (session->state == state)
        return;
    session->state = state;
    emit_state(session);
}

static void emit_error(cp_session_t *session, cp_session_error_t error)
{
    session->last_error = error;
    emit_event(session, CP_SESSION_EVENT_ERROR, error, CP_BOOT_EV_NONE,
               CP_BOOT_OK, CP_LINK_RESET_NONE);
}

static void emit_receiver(cp_session_t *session)
{
    emit_event(session, CP_SESSION_EVENT_RECEIVER, CP_SESSION_OK,
               CP_BOOT_EV_NONE, CP_BOOT_OK, CP_LINK_RESET_NONE);
}

static bool clock_accept(cp_session_t *session, uint64_t now_ms)
{
    if (now_ms < session->last_now_ms) {
        session->stats.clock_rollbacks++;
        emit_error(session, CP_SESSION_ERR_CLOCK_ROLLBACK);
        return false;
    }
    session->last_now_ms = now_ms;
    session->now_ms = now_ms;
    return true;
}

static cp_session_error_t map_link_result(int result)
{
    switch (result) {
    case CP_LINK_OK:
        return CP_SESSION_OK;
    case CP_LINK_ERR_STATE:
        return CP_SESSION_ERR_STATE;
    case CP_LINK_ERR_BACKPRESSURE:
        return CP_SESSION_ERR_LINK_LIMIT;
    case CP_LINK_ERR_TOO_LARGE:
    case CP_LINK_ERR_NO_SESSION:
        return CP_SESSION_ERR_LINK_LIMIT;
    case CP_LINK_ERR_INVALID:
    default:
        return CP_SESSION_ERR_ARGUMENT;
    }
}

static cp_session_error_t map_boot_error(cp_boot_error error)
{
    switch (error) {
    case CP_BOOT_OK:
        return CP_SESSION_OK;
    case CP_BOOT_ERR_ARGUMENT:
        return CP_SESSION_ERR_ARGUMENT;
    case CP_BOOT_ERR_CONFIG:
        return CP_SESSION_ERR_CONFIG;
    case CP_BOOT_ERR_WIRE:
        return CP_SESSION_ERR_CSM_MALFORMED;
    case CP_BOOT_ERR_ORDER:
    case CP_BOOT_ERR_IDENTITY:
    case CP_BOOT_ERR_BUFFER:
        return CP_SESSION_ERR_BOOTSTRAP;
    case CP_BOOT_ERR_IDENTIFICATION:
        return CP_SESSION_ERR_IDENTIFICATION_REJECTED;
    case CP_BOOT_ERR_AUTH_PROVIDER:
        return CP_SESSION_ERR_PROVIDER;
    case CP_BOOT_ERR_AUTH_FAILED:
        return CP_SESSION_ERR_AUTH_FAILED;
    case CP_BOOT_ERR_POLICY:
        return CP_SESSION_ERR_POLICY;
    case CP_BOOT_ERR_TIMEOUT:
        return CP_SESSION_ERR_TIMEOUT;
    default:
        return CP_SESSION_ERR_BOOTSTRAP;
    }
}

static cp_session_error_t map_link_reset(cp_link_reset_reason_t reason)
{
    switch (reason) {
    case CP_LINK_RESET_TX_BACKPRESSURE:
        return CP_SESSION_ERR_LINK_LIMIT;
    case CP_LINK_RESET_CANCELLED:
        return CP_SESSION_ERR_CANCELLED;
    default:
        return CP_SESSION_ERR_LINK;
    }
}

static void tx_pop(cp_session_t *session)
{
    cp_session_tx_slot_t *slot = &session->tx_slots[session->tx_head];

    memset(slot, 0, sizeof(*slot));
    session->tx_head =
        (uint8_t)(((unsigned)session->tx_head + 1u) %
                  (unsigned)CP_SESSION_TX_QUEUE);
    if (session->tx_count > 0u)
        session->tx_count--;
    if (session->tx_count == 0u)
        session->tx_head = 0u;
}

/* Zeroes queued CSM bytes (they can contain AP credentials) and reports
 * whether any partially transmitted reply existed. */
static uint8_t tx_drop_all(cp_session_t *session, bool *partial)
{
    uint8_t dropped = session->tx_count;
    uint8_t i;

    if (partial != NULL) {
        *partial = false;
        for (i = 0; i < session->tx_count; i++) {
            unsigned index = ((unsigned)session->tx_head + (unsigned)i) %
                             (unsigned)CP_SESSION_TX_QUEUE;
            if (session->tx_slots[index].sent != 0u)
                *partial = true;
        }
    }
    memset(session->tx_slots, 0, sizeof(session->tx_slots));
    session->tx_head = 0u;
    session->tx_count = 0u;
    return dropped;
}

static void rx_drop_all(cp_session_t *session)
{
    memset(session->rx_buf, 0, sizeof(session->rx_buf));
    session->rx_len = 0u;
    session->rx_partial_seen = false;
    session->rx_current_fragmented = false;
    session->cb_rx_overflow = false;
}

static void clear_receiver(cp_session_t *session, bool notify)
{
    session->receiver_policy_deadline_ms = 0u;
    session->receiver_policy_armed = false;
    /* Drop the bootstrap offer hold at the same boundary as the wrapper
     * observation.  The offer itself keeps its ordinary bounded deadline;
     * releasing the hold never bumps a generation or drops a queued reply. */
    (void)cp_bootstrap_release_offer_hold(&session->boot,
                                          session->last_now_ms);
    if (!session->receiver_active)
        return;
    session->receiver_active = false;
    session->receiver_generation = 0u;
    session->receiver_ap_generation = 0u;
    if (notify)
        emit_receiver(session);
}

static bool receiver_inputs_valid(const cp_session_t *session)
{
    return cp_link_state(&session->link) == CP_LINK_STATE_NORMAL &&
           cp_bootstrap_auth_state(&session->boot) ==
               CP_BOOT_AUTH_SUCCEEDED &&
           cp_bootstrap_state(&session->boot) != CP_BOOT_STATE_FAILED &&
           cp_bootstrap_state(&session->boot) !=
               CP_BOOT_STATE_DISCONNECTED &&
           cp_bootstrap_ap_state(&session->boot) == CP_BOOT_AP_READY &&
           cp_bootstrap_offer_held(&session->boot) &&
           session->receiver_generation == cp_link_generation(&session->link) &&
           session->receiver_ap_generation ==
               session->ap_readback_generation;
}

static bool boot_event_clears_receiver(const cp_session_t *session,
                                       cp_boot_event event)
{
    switch (event) {
    case CP_BOOT_EV_AUTH_FAILED:
    case CP_BOOT_EV_IDENTIFICATION_REJECTED:
    case CP_BOOT_EV_ERROR:
    case CP_BOOT_EV_CANCELLED:
    case CP_BOOT_EV_TRANSPORT_RESET:
    case CP_BOOT_EV_CONSENT_DENIED:
    case CP_BOOT_EV_USB_ACTIVE:
    case CP_BOOT_EV_AVAILABILITY_FALSE:
    case CP_BOOT_EV_START_REFUSED:
        return true;
    case CP_BOOT_EV_WIRELESS_STATUS:
        return session->boot.wireless_update_known &&
               !session->boot.wireless_update_available;
    case CP_BOOT_EV_PHONE_IDENTITY:
        return !session->boot.offer_active;
    default:
        return false;
    }
}

static void clear_receiver_if_invalid(cp_session_t *session)
{
    if (session->receiver_active && !receiver_inputs_valid(session))
        clear_receiver(session, true);
}

static bool bounded_text_equal(const char *a, const char *b, size_t max_len)
{
    size_t i;

    if (a == NULL || b == NULL)
        return a == b;
    for (i = 0; i <= max_len; i++) {
        if (a[i] != b[i])
            return false;
        if (a[i] == '\0')
            return true;
    }
    return false;
}

static bool id_bounded(const char *id)
{
    size_t i;

    if (id == NULL || id[0] == '\0')
        return false;
    for (i = 0; i <= CP_WIRE_MAX_TRANSPORT_ID; i++) {
        if (id[i] == '\0')
            return i != 0u;
    }
    return false;
}

static bool id_equal_ci(const char *a, const char *b)
{
    size_t i = 0;

    if (a == NULL || b == NULL)
        return false;
    while (a[i] != '\0' && b[i] != '\0') {
        char ca = a[i];
        char cb = b[i];

        if (ca >= 'A' && ca <= 'Z')
            ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z')
            cb = (char)(cb - 'A' + 'a');
        if (ca != cb)
            return false;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static void set_state_from_boot(cp_session_t *session,
                                const cp_boot_result *result)
{
    cp_session_state_t state;

    switch (result->event) {
    case CP_BOOT_EV_IDENTIFICATION_SENT:
    case CP_BOOT_EV_IDENTIFICATION_RETRY:
        state = CP_SESSION_STATE_IDENTIFYING;
        break;
    case CP_BOOT_EV_IDENTIFIED:
        state = CP_SESSION_STATE_IDENTIFIED;
        break;
    case CP_BOOT_EV_AUTH_CERT_SENT:
    case CP_BOOT_EV_AUTH_RESPONSE_SENT:
    case CP_BOOT_EV_AUTH_PENDING:
        state = CP_SESSION_STATE_AUTHENTICATING;
        break;
    case CP_BOOT_EV_AUTHENTICATED:
        state = CP_SESSION_STATE_AUTHENTICATED;
        break;
    case CP_BOOT_EV_START_SENT:
        state = CP_SESSION_STATE_AWAITING_RECEIVER;
        break;
    case CP_BOOT_EV_IDENTIFICATION_REJECTED:
    case CP_BOOT_EV_AUTH_FAILED:
    case CP_BOOT_EV_ERROR:
        state = CP_SESSION_STATE_FAILED;
        break;
    case CP_BOOT_EV_CANCELLED:
        state = CP_SESSION_STATE_CANCELLED;
        break;
    case CP_BOOT_EV_TRANSPORT_RESET:
        state = CP_SESSION_STATE_DISCONNECTED;
        break;
    default:
        switch (result->state) {
        case CP_BOOT_STATE_FAILED:
            state = CP_SESSION_STATE_FAILED;
            break;
        case CP_BOOT_STATE_AWAITING_RECEIVER:
            state = CP_SESSION_STATE_AWAITING_RECEIVER;
            break;
        case CP_BOOT_STATE_AUTHENTICATED:
            state = CP_SESSION_STATE_AUTHENTICATED;
            break;
        case CP_BOOT_STATE_IDENTIFIED:
            state = ((session->boot.auth_request.op != CP_BOOT_AUTH_OP_NONE &&
                      session->boot.auth_request.generation == session->boot.generation) ||
                     session->boot.auth_state == CP_BOOT_AUTH_CERT_SENT ||
                     session->boot.auth_state == CP_BOOT_AUTH_RESPONSE_SENT) ?
                    CP_SESSION_STATE_AUTHENTICATING : CP_SESSION_STATE_IDENTIFIED;
            break;
        case CP_BOOT_STATE_IDENTIFYING:
            state = CP_SESSION_STATE_IDENTIFYING;
            break;
        case CP_BOOT_STATE_DISCONNECTED:
            if (session->started &&
                cp_link_state(&session->link) != CP_LINK_STATE_DEAD) {
                state = CP_SESSION_STATE_STARTING;
            } else if (session->state == CP_SESSION_STATE_IDLE ||
                       session->state == CP_SESSION_STATE_CANCELLED ||
                       session->state == CP_SESSION_STATE_FAILED) {
                /* Publishing AP readback is not a transport transition. Keep
                 * inactive/cancelled/failed lifecycle states intact. */
                state = session->state;
            } else {
                state = CP_SESSION_STATE_DISCONNECTED;
            }
            break;
        default:
            state = session->state;
            break;
        }
        break;
    }
    set_state(session, state);
}

static cp_session_state_t state_from_current_boot(const cp_session_t *session)
{
    switch (cp_bootstrap_state(&session->boot)) {
    case CP_BOOT_STATE_FAILED:
        return CP_SESSION_STATE_FAILED;
    case CP_BOOT_STATE_AWAITING_RECEIVER:
        return CP_SESSION_STATE_AWAITING_RECEIVER;
    case CP_BOOT_STATE_AUTHENTICATED:
        return CP_SESSION_STATE_AUTHENTICATED;
    case CP_BOOT_STATE_IDENTIFIED:
        return ((session->boot.auth_request.op != CP_BOOT_AUTH_OP_NONE &&
                 session->boot.auth_request.generation == session->boot.generation) ||
                session->boot.auth_state == CP_BOOT_AUTH_CERT_SENT ||
                session->boot.auth_state == CP_BOOT_AUTH_RESPONSE_SENT) ?
               CP_SESSION_STATE_AUTHENTICATING : CP_SESSION_STATE_IDENTIFIED;
    case CP_BOOT_STATE_IDENTIFYING:
        return CP_SESSION_STATE_IDENTIFYING;
    case CP_BOOT_STATE_DISCONNECTED:
    default:
        return session->started &&
                       cp_link_state(&session->link) != CP_LINK_STATE_DEAD
                   ? CP_SESSION_STATE_STARTING
                   : CP_SESSION_STATE_DISCONNECTED;
    }
}

static cp_session_error_t abort_session(cp_session_t *session,
                                        cp_session_error_t error,
                                        uint64_t now_ms);

static cp_session_error_t sync_link_generation(cp_session_t *session,
                                               uint64_t now_ms)
{
    uint32_t generation = cp_link_generation(&session->link);
    uint32_t old_generation = session->bound_generation;
    cp_link_reset_reason_t reason = session->cb_reset_reason;
    bool partial = false;
    uint8_t dropped;
    bool was_receiver;
    cp_boot_result result;

    if (generation == session->bound_generation)
        return CP_SESSION_OK;

    session->bound_generation = generation;
    if (generation != old_generation)
        session->stats.generation_resets++;

    dropped = tx_drop_all(session, &partial);
    session->stats.csm_stale_replies_dropped += dropped;
    (void)partial; /* The link itself is already gone; all pending is moot. */
    rx_drop_all(session);

    was_receiver = session->receiver_active;
    if (was_receiver)
        clear_receiver(session, false);
    session->receiver_generation = 0u;

    if (reason == CP_LINK_RESET_NONE)
        reason = cp_link_reset_reason(&session->link);
    session->last_error = map_link_reset(reason);
    session->started = false;
    session->cb_link_reset = false;
    session->cb_link_up = false;
    session->cb_link_state = cp_link_state(&session->link);

    result = cp_bootstrap_transport_reset(&session->boot, now_ms);
    (void)result;

    session->state = CP_SESSION_STATE_DISCONNECTED;
    session->stats.link_reset_events++;
    emit_event(session, CP_SESSION_EVENT_LINK_RESET, session->last_error,
               CP_BOOT_EV_TRANSPORT_RESET, CP_BOOT_OK, reason);
    if (was_receiver)
        emit_receiver(session);
    emit_state(session);
    refresh_link_stats(session);
    return CP_SESSION_OK;
}

static cp_session_error_t abort_session(cp_session_t *session,
                                        cp_session_error_t error,
                                        uint64_t now_ms)
{
    uint32_t old_generation = cp_link_generation(&session->link);
    bool partial = false;
    uint8_t dropped;
    bool was_receiver;
    cp_boot_result result;

    dropped = tx_drop_all(session, &partial);
    session->stats.csm_stale_replies_dropped += dropped;
    rx_drop_all(session);
    was_receiver = session->receiver_active;
    if (was_receiver)
        clear_receiver(session, false);
    session->receiver_generation = 0u;
    session->last_error = error;

    if (cp_link_state(&session->link) != CP_LINK_STATE_DEAD)
        (void)cp_link_abort(&session->link, now_ms,
                            CP_LINK_RESET_LOCAL_ABORT);
    session->bound_generation = cp_link_generation(&session->link);
    if (session->bound_generation != old_generation)
        session->stats.generation_resets++;

    session->cb_link_reset = false;
    session->cb_link_up = false;
    session->cb_link_state = cp_link_state(&session->link);
    result = cp_bootstrap_transport_reset(&session->boot, now_ms);
    (void)result;
    session->started = false;

    session->state = CP_SESSION_STATE_FAILED;
    session->stats.link_reset_events++;
    emit_event(session, CP_SESSION_EVENT_LINK_RESET, error,
               CP_BOOT_EV_TRANSPORT_RESET, CP_BOOT_OK,
               CP_LINK_RESET_LOCAL_ABORT);
    emit_error(session, error);
    if (was_receiver)
        emit_receiver(session);
    emit_state(session);
    refresh_link_stats(session);
    return error;
}

static cp_session_error_t queue_reply(cp_session_t *session,
                                      const uint8_t *bytes, size_t len,
                                      uint32_t boot_generation,
                                      uint64_t now_ms)
{
    unsigned index;
    cp_session_tx_slot_t *slot;

    if (bytes == NULL || len == 0u || len > CP_WIRE_MAX_FRAME)
        return abort_session(session, CP_SESSION_ERR_BOOTSTRAP, now_ms);
    if (session->tx_count >= CP_SESSION_TX_QUEUE) {
        session->stats.csm_tx_queue_overflows++;
        return abort_session(session, CP_SESSION_ERR_TX_QUEUE_OVERFLOW,
                             now_ms);
    }

    index = ((unsigned)session->tx_head + (unsigned)session->tx_count) %
            (unsigned)CP_SESSION_TX_QUEUE;
    slot = &session->tx_slots[index];
    memcpy(slot->bytes, bytes, len);
    slot->len = (uint16_t)len;
    slot->sent = 0u;
    slot->boot_generation = boot_generation;
    session->tx_count++;
    session->stats.csm_replies_queued++;
    session->stats.csm_bytes_queued += (uint32_t)len;
    return CP_SESSION_OK;
}

static cp_session_error_t flush_tx(cp_session_t *session, uint64_t now_ms)
{
    for (;;) {
        cp_session_tx_slot_t *slot;
        uint32_t boot_generation;
        size_t max_payload;
        size_t remaining;
        size_t segment;
        int result;

        if (session->tx_count == 0u)
            return CP_SESSION_OK;

        slot = &session->tx_slots[session->tx_head];
        boot_generation = cp_bootstrap_generation(&session->boot);
        if (slot->boot_generation != boot_generation) {
            if (slot->sent != 0u) {
                /* A partially delivered old reply must not be completed:
                 * reset the link so the peer discards the truncated CSM.
                 * abort_session() counts and clears the remaining queue. */
                return abort_session(session, CP_SESSION_ERR_TX_STALE,
                                     now_ms);
            }
            session->stats.csm_stale_replies_dropped++;
            tx_pop(session);
            continue;
        }

        if (cp_link_state(&session->link) != CP_LINK_STATE_NORMAL)
            return CP_SESSION_OK;

        if (slot->sent >= slot->len) {
            session->stats.csm_replies_sent++;
            tx_pop(session);
            continue;
        }

        max_payload = cp_link_max_payload(&session->link);
        if (max_payload == 0u)
            return abort_session(session, CP_SESSION_ERR_LINK_LIMIT, now_ms);

        remaining = (size_t)slot->len - (size_t)slot->sent;
        segment = min_size(remaining, max_payload);
        result = cp_link_send(&session->link, CP_LINK_SESSION_CONTROL,
                              slot->bytes + slot->sent, segment, now_ms);
        if (result == CP_LINK_OK) {
            slot->sent = (uint16_t)(slot->sent + (uint16_t)segment);
            session->stats.csm_segments_sent++;
            session->stats.csm_bytes_sent += (uint32_t)segment;
            if (slot->sent == slot->len) {
                session->stats.csm_replies_sent++;
                tx_pop(session);
            }
            continue;
        }
        if (result == CP_LINK_ERR_BACKPRESSURE) {
            session->stats.link_backpressure_retries++;
            return CP_SESSION_OK;
        }
        if (result == CP_LINK_ERR_STATE) {
            if (cp_link_state(&session->link) == CP_LINK_STATE_DEAD)
                return sync_link_generation(session, now_ms);
            return CP_SESSION_OK;
        }
        return abort_session(session,
                             result == CP_LINK_ERR_INVALID
                                 ? CP_SESSION_ERR_BOOTSTRAP
                                 : CP_SESSION_ERR_LINK_LIMIT,
                             now_ms);
    }
}

static cp_session_error_t receiver_policy_check(cp_session_t *session,
                                                  uint64_t now_ms)
{
    cp_boot_event cause = CP_BOOT_EV_NONE;
    cp_boot_consent consent;
    bool partial = false;
    uint8_t dropped;
    cp_boot_result result;

    if (!session->receiver_active)
        return CP_SESSION_OK;
    if (!receiver_inputs_valid(session)) {
        clear_receiver_if_invalid(session);
        return CP_SESSION_OK;
    }

    if (session->policy_consent == NULL) {
        cause = CP_BOOT_EV_CONSENT_DENIED;
    } else {
        consent = session->policy_consent(session->policy_context, now_ms);
        if (consent != CP_BOOT_CONSENT_GRANTED &&
            consent != CP_BOOT_CONSENT_PENDING)
            cause = CP_BOOT_EV_CONSENT_DENIED;
    }
    if (cause == CP_BOOT_EV_NONE && session->policy_usb_active != NULL &&
        session->policy_usb_active(
            session->policy_context,
            session->boot.have_phone_bluetooth_id
                ? session->boot.phone_bluetooth_id
                : "",
            session->boot.have_phone_usb_id ? session->boot.phone_usb_id
                                            : "")) {
        cause = CP_BOOT_EV_USB_ACTIVE;
    }
    if (cause == CP_BOOT_EV_NONE && session->boot.wireless_update_known &&
        !session->boot.wireless_update_available)
        cause = CP_BOOT_EV_WIRELESS_STATUS;

    if (cause == CP_BOOT_EV_NONE) {
        session->receiver_policy_deadline_ms =
            add_ms(now_ms, CP_SESSION_POLICY_RECHECK_MS);
        session->receiver_policy_armed = true;
        return CP_SESSION_OK;
    }

    session->stats.policy_refusals++;
    clear_receiver(session, true);
    rx_drop_all(session);
    dropped = tx_drop_all(session, &partial);
    session->stats.csm_stale_replies_dropped += dropped;
    if (partial)
        return abort_session(session, CP_SESSION_ERR_TX_STALE, now_ms);

    result = cp_bootstrap_cancel(&session->boot, now_ms);
    (void)result;
    session->last_error = CP_SESSION_ERR_POLICY;
    session->stats.boot_events++;
    session->stats.boot_errors++;
    set_state(session, state_from_current_boot(session));
    emit_event(session, CP_SESSION_EVENT_BOOT, CP_SESSION_ERR_POLICY, cause,
               CP_BOOT_ERR_POLICY, CP_LINK_RESET_NONE);
    emit_error(session, CP_SESSION_ERR_POLICY);
    return CP_SESSION_OK;
}

static cp_session_error_t handle_boot_result(cp_session_t *session,
                                             cp_boot_result result,
                                             uint64_t old_boot_generation,
                                             const uint8_t *out,
                                             size_t out_len, uint64_t now_ms)
{
    cp_session_error_t mapped = map_boot_error(result.error);
    bool partial = false;
    uint8_t dropped;

    session->stats.boot_events++;
    if (result.error != CP_BOOT_OK)
        session->stats.boot_errors++;

    if (result.event == CP_BOOT_EV_AUTH_FAILED)
        session->stats.auth_failures++;
    if (result.event == CP_BOOT_EV_IDENTIFICATION_REJECTED)
        session->stats.identification_rejections++;
    if (result.event == CP_BOOT_EV_CONSENT_DENIED ||
        result.event == CP_BOOT_EV_USB_ACTIVE)
        session->stats.policy_refusals++;

    if (cp_bootstrap_generation(&session->boot) != old_boot_generation) {
        dropped = tx_drop_all(session, &partial);
        session->stats.csm_stale_replies_dropped += dropped;
        if (partial)
            return abort_session(session, CP_SESSION_ERR_TX_STALE, now_ms);
    }

    if (out_len != result.out_len)
        return abort_session(session, CP_SESSION_ERR_BOOTSTRAP, now_ms);

    if (result.error == CP_BOOT_ERR_WIRE) {
        session->stats.csm_malformed++;
        return abort_session(session, CP_SESSION_ERR_CSM_MALFORMED, now_ms);
    }
    if (result.error == CP_BOOT_ERR_BUFFER)
        return abort_session(session, CP_SESSION_ERR_BOOTSTRAP, now_ms);

    if (mapped != CP_SESSION_OK)
        session->last_error = mapped;

    set_state_from_boot(session, &result);

    if (result.out_len != 0u) {
        cp_session_error_t err = queue_reply(
            session, out, result.out_len, result.generation, now_ms);

        if (err != CP_SESSION_OK)
            return err;
    }

    emit_event(session, CP_SESSION_EVENT_BOOT, mapped, result.event,
               result.error, CP_LINK_RESET_NONE);
    if (session->receiver_active &&
        boot_event_clears_receiver(session, result.event))
        clear_receiver(session, true);
    clear_receiver_if_invalid(session);
    return CP_SESSION_OK;
}

static bool rx_front_incomplete(const cp_session_t *session)
{
    uint16_t total;

    if (session->rx_len == 0u)
        return false;
    if (session->rx_len < 4u)
        return true;
    if (session->rx_buf[0] != 0x40u || session->rx_buf[1] != 0x40u)
        return false;
    total = be16(session->rx_buf + 2u);
    if (total < CP_SESSION_CSM_HEADER_LEN || total > CP_WIRE_MAX_FRAME)
        return false;
    return (size_t)total > session->rx_len;
}

static cp_session_error_t process_rx(cp_session_t *session, uint64_t now_ms)
{
    uint8_t out[CP_WIRE_MAX_FRAME];
    size_t processed = 0u;

    while (session->rx_len > 0u) {
        uint16_t total;
        uint64_t old_boot_generation;
        cp_boot_result result;
        cp_session_error_t err;

        if (session->rx_buf[0] != 0x40u ||
            (session->rx_len >= 2u && session->rx_buf[1] != 0x40u)) {
            session->stats.csm_malformed++;
            return abort_session(session, CP_SESSION_ERR_CSM_MALFORMED,
                                 now_ms);
        }
        if (session->rx_len < 4u)
            break;
        total = be16(session->rx_buf + 2u);
        if (total < CP_SESSION_CSM_HEADER_LEN || total > CP_WIRE_MAX_FRAME) {
            session->stats.csm_malformed++;
            return abort_session(session, CP_SESSION_ERR_CSM_MALFORMED,
                                 now_ms);
        }
        if ((size_t)total > session->rx_len) {
            session->rx_partial_seen = true;
            break;
        }

        old_boot_generation = cp_bootstrap_generation(&session->boot);
        result = cp_bootstrap_receive(&session->boot, session->rx_buf, total,
                                      now_ms, out, sizeof(out));
        err = handle_boot_result(session, result, old_boot_generation, out,
                                 result.out_len, now_ms);
        if (err != CP_SESSION_OK)
            return err;

        session->stats.csm_frames_rx++;
        session->stats.csm_bytes_rx += total;
        if (session->rx_current_fragmented) {
            session->stats.csm_fragmented_frames++;
            session->rx_current_fragmented = false;
        }
        if (processed != 0u)
            session->stats.csm_coalesced_frames++;
        processed++;

        memmove(session->rx_buf, session->rx_buf + total,
                session->rx_len - total);
        session->rx_len -= total;
        session->rx_partial_seen = false;
        if (session->rx_len == 0u) {
            session->rx_current_fragmented = false;
            session->rx_partial_seen = false;
        }
    }
    return CP_SESSION_OK;
}

static cp_session_error_t service_after_link(cp_session_t *session,
                                             uint64_t now_ms,
                                             bool tick_bootstrap)
{
    cp_session_error_t err;

    if (session->cb_rx_overflow) {
        session->stats.csm_rx_overflows++;
        return abort_session(session, CP_SESSION_ERR_RX_OVERFLOW, now_ms);
    }

    if (session->cb_link_reset ||
        cp_link_generation(&session->link) != session->bound_generation) {
        err = sync_link_generation(session, now_ms);
        if (err != CP_SESSION_OK)
            return err;
    }

    if (session->cb_link_up) {
        session->cb_link_up = false;
        session->stats.link_up_events++;
        emit_event(session, CP_SESSION_EVENT_LINK_UP, CP_SESSION_OK,
                   CP_BOOT_EV_NONE, CP_BOOT_OK, CP_LINK_RESET_NONE);
    }

    err = process_rx(session, now_ms);
    if (err != CP_SESSION_OK)
        return err;

    if (tick_bootstrap && !session->receiver_active) {
        uint8_t out[CP_WIRE_MAX_FRAME];
        uint64_t old_boot_generation =
            cp_bootstrap_generation(&session->boot);
        cp_boot_result result = cp_bootstrap_tick(&session->boot, now_ms, out,
                                                  sizeof(out));

        err = handle_boot_result(session, result, old_boot_generation, out,
                                 result.out_len, now_ms);
        if (err != CP_SESSION_OK)
            return err;
    }

    err = flush_tx(session, now_ms);
    if (err != CP_SESSION_OK)
        return err;

    if (cp_link_generation(&session->link) != session->bound_generation) {
        err = sync_link_generation(session, now_ms);
        if (err != CP_SESSION_OK)
            return err;
    }

    if (session->receiver_active && session->receiver_policy_armed &&
        now_ms >= session->receiver_policy_deadline_ms) {
        err = receiver_policy_check(session, now_ms);
        if (err != CP_SESSION_OK)
            return err;
    }

    clear_receiver_if_invalid(session);
    refresh_link_stats(session);
    return CP_SESSION_OK;
}

static void session_link_callback(void *user, const cp_link_event_t *event)
{
    cp_session_t *session = (cp_session_t *)user;

    switch (event->type) {
    case CP_LINK_EVENT_SESSION_DATA:
        if (event->session_type != CP_LINK_SESSION_CONTROL) {
            session->stats.unsupported_session_payloads++;
            return;
        }
        if (session->rx_len + (size_t)event->data_len >
            sizeof(session->rx_buf)) {
            session->cb_rx_overflow = true;
            return;
        }
        if (session->rx_len == 0u)
            session->rx_current_fragmented = false;
        else if (rx_front_incomplete(session))
            session->rx_current_fragmented = true;
        if (event->data_len != 0u)
            memcpy(session->rx_buf + session->rx_len, event->data,
                   event->data_len);
        session->rx_len += event->data_len;
        session->stats.csm_link_payloads_rx++;
        break;
    case CP_LINK_EVENT_STATE:
        session->cb_link_state = event->state;
        if (event->state == CP_LINK_STATE_NORMAL)
            session->cb_link_up = true;
        break;
    case CP_LINK_EVENT_RESET:
        session->cb_link_reset = true;
        session->cb_reset_reason = event->reset_reason;
        break;
    case CP_LINK_EVENT_WRITABLE:
    default:
        break;
    }
}

static void refresh_link_stats(cp_session_t *session)
{
    session->stats.link = *cp_link_stats(&session->link);
}

static bool session_link_config_acceptable(const cp_link_config_t *cfg)
{
    size_t sessions = 1u;
    size_t syn_frame_len;

    if (cfg == NULL)
        return false;
    if (cfg->external_accessory_session_id != 0u)
        sessions++;
    if (cfg->file_transfer_session_id != 0u)
        sessions++;
    /* header 9 + LSP (10 + 3 per session) + payload checksum 1 */
    syn_frame_len = 20u + 3u * sessions;
    if ((size_t)cfg->max_payload + 10u < syn_frame_len)
        return false;
    return true;
}

void cp_session_config_default(cp_session_config_t *config)
{
    if (config == NULL)
        return;
    memset(config, 0, sizeof(*config));
    /* The default wireless profile follows the observed live Bluetooth
     * advertisement. Explicit callers may override any link field. */
    cp_link_config_live_bluetooth(&config->link);
    /* config.boot must be filled by the caller with real identity/AP data. */
}

cp_session_error_t cp_session_init(cp_session_t *session,
                                   const cp_session_config_t *config,
                                   cp_session_event_fn on_event, void *user,
                                   uint64_t now_ms)
{
    int link_result;
    cp_boot_error boot_error;
    cp_boot_result reset_result;

    if (session == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    memset(session, 0, sizeof(*session));
    if (config == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    if (!session_link_config_acceptable(&config->link))
        return CP_SESSION_ERR_CONFIG;
    link_result = cp_link_init(&session->link, &config->link,
                               session_link_callback, session);
    if (link_result != CP_LINK_OK)
        return CP_SESSION_ERR_CONFIG;
    boot_error = cp_bootstrap_init(&session->boot, &config->boot);
    if (boot_error != CP_BOOT_OK) {
        memset(session, 0, sizeof(*session));
        return boot_error == CP_BOOT_ERR_ARGUMENT ? CP_SESSION_ERR_ARGUMENT
                                                  : CP_SESSION_ERR_CONFIG;
    }

    session->on_event = on_event;
    session->user = user;
    session->state = CP_SESSION_STATE_IDLE;
    session->last_error = CP_SESSION_OK;
    session->initialized = true;
    session->started = false;
    session->now_ms = now_ms;
    session->last_now_ms = now_ms;
    session->bound_generation = cp_link_generation(&session->link);

    /* Bind the bootstrap clock to the construction clock. No traffic. */
    reset_result = cp_bootstrap_transport_reset(&session->boot, now_ms);
    (void)reset_result;
    return CP_SESSION_OK;
}

cp_session_error_t cp_session_start(cp_session_t *session,
                                    cp_link_role_t role, uint64_t now_ms)
{
    cp_link_state_t link_state;
    int result;
    uint32_t old_generation;
    uint32_t generation;
    cp_boot_result reset_result;
    bool was_receiver;

    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;

    link_state = cp_link_state(&session->link);
    if (link_state == CP_LINK_STATE_DETECT ||
        link_state == CP_LINK_STATE_NEGOTIATE ||
        link_state == CP_LINK_STATE_NORMAL)
        return CP_SESSION_ERR_STATE;

    was_receiver = session->receiver_active;
    if (was_receiver)
        clear_receiver(session, true);
    session->receiver_generation = 0u;
    session->receiver_policy_deadline_ms = 0u;
    session->receiver_policy_armed = false;
    session->stats.csm_stale_replies_dropped += tx_drop_all(session, NULL);
    rx_drop_all(session);
    session->cb_rx_overflow = false;
    session->cb_link_reset = false;
    session->cb_link_up = false;
    session->cb_reset_reason = CP_LINK_RESET_NONE;

    old_generation = cp_link_generation(&session->link);
    result = cp_link_start(&session->link, role, now_ms);
    if (result != CP_LINK_OK)
        return map_link_result(result);
    generation = cp_link_generation(&session->link);
    session->bound_generation = generation;
    if (generation != old_generation)
        session->stats.generation_resets++;

    reset_result = cp_bootstrap_transport_reset(&session->boot, now_ms);
    (void)reset_result;

    session->started = true;
    session->last_error = CP_SESSION_OK;
    session->state = CP_SESSION_STATE_STARTING;
    emit_state(session);
    refresh_link_stats(session);
    return CP_SESSION_OK;
}

cp_session_error_t cp_session_feed(cp_session_t *session, const void *data,
                                   size_t len, uint64_t now_ms)
{
    const uint8_t *bytes = (const uint8_t *)data;
    cp_session_error_t err = CP_SESSION_OK;
    size_t i;

    if (session == NULL || !session->initialized ||
        (data == NULL && len != 0u))
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (cp_link_state(&session->link) == CP_LINK_STATE_IDLE ||
        cp_link_state(&session->link) == CP_LINK_STATE_DEAD)
        return CP_SESSION_ERR_STATE;

    /* Feed one byte at a time. A single byte completes at most one link frame,
     * so ordinary frames are serviced between bytes and caller input size is
     * not a buffer bound. A gap-closing frame can still invoke callbacks for
     * the current frame plus all CP_LINK_MAX_REORDER contiguous reordered
     * frames in one cp_link_feed call; CP_SESSION_RX_BUFFER is sized for that
     * bounded burst (one partial CSM plus MAX_REORDER+1 maximum payloads).
     * The loop is linear in the caller's byte count; each iteration consumes
     * one byte. */
    for (i = 0; i < len; i++) {
        int result = cp_link_feed(&session->link, bytes + i, 1u, now_ms);

        if (result != CP_LINK_OK) {
            err = map_link_result(result);
            break;
        }
        err = service_after_link(session, now_ms, false);
        if (err != CP_SESSION_OK)
            break;
        if (cp_link_state(&session->link) == CP_LINK_STATE_DEAD)
            break;
    }
    if (err != CP_SESSION_OK)
        return err;

    /* Bootstrap tick is deliberately once per public feed, not per byte. */
    return service_after_link(session, now_ms, true);
}

cp_session_error_t cp_session_tick(cp_session_t *session, uint64_t now_ms)
{
    int result;
    cp_session_error_t err;

    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (!session->started)
        return CP_SESSION_OK;

    result = cp_link_tick(&session->link, now_ms);
    if (result != CP_LINK_OK && result != CP_LINK_ERR_STATE)
        return map_link_result(result);

    err = service_after_link(session, now_ms, true);
    return err;
}

uint64_t cp_session_next_deadline(const cp_session_t *session)
{
    uint64_t deadline;

    if (session == NULL || !session->initialized)
        return UINT64_MAX;

    deadline = cp_link_next_deadline(&session->link);
    if (session->boot.auth_request.op != CP_BOOT_AUTH_OP_NONE &&
        session->boot.auth_request.generation == session->boot.generation &&
        session->boot.auth_request.deadline_ms < deadline)
        deadline = session->boot.auth_request.deadline_ms;
    if (session->boot.pending_start &&
        session->boot.pending_deadline_ms != 0u &&
        session->boot.pending_deadline_ms < deadline)
        deadline = session->boot.pending_deadline_ms;
    if (session->receiver_active) {
        if (session->receiver_policy_armed &&
            session->receiver_policy_deadline_ms < deadline)
            deadline = session->receiver_policy_deadline_ms;
    } else if (session->boot.offer_active &&
               session->boot.offer_deadline_ms != 0u &&
               session->boot.offer_deadline_ms < deadline) {
        deadline = session->boot.offer_deadline_ms;
    }
    return deadline;
}

cp_session_error_t cp_session_set_auth_provider(
    cp_session_t *session, const cp_boot_auth_provider *provider)
{
    cp_boot_error error;

    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    error = cp_bootstrap_set_auth_provider(&session->boot, provider);
    return error == CP_BOOT_OK ? CP_SESSION_OK : CP_SESSION_ERR_ARGUMENT;
}

cp_session_error_t cp_session_set_async_auth(cp_session_t *session, bool enabled)
{
    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    return cp_bootstrap_set_async_auth(&session->boot, enabled) == CP_BOOT_OK ?
        CP_SESSION_OK : CP_SESSION_ERR_STATE;
}

bool cp_session_auth_request(const cp_session_t *session,
                             cp_boot_auth_request *request)
{
    return session != NULL && session->initialized &&
           cp_bootstrap_auth_request(&session->boot, request);
}

cp_session_error_t cp_session_auth_complete(
    cp_session_t *session, const cp_boot_auth_request *request, bool success,
    const uint8_t *blob, size_t blob_len, uint64_t now_ms)
{
    cp_boot_auth_request pending;
    uint8_t out[CP_WIRE_MAX_FRAME];
    cp_boot_result result;
    cp_session_error_t err;
    uint64_t generation;
    if (request == NULL || !cp_session_auth_request(session, &pending))
        return CP_SESSION_ERR_STATE;
    if (request->generation != pending.generation ||
        request->token != pending.token || request->op != pending.op)
        return CP_SESSION_ERR_GENERATION_MISMATCH;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    generation = session->boot.generation;
    result = cp_bootstrap_auth_complete(&session->boot, request->generation,
        request->token, request->op, success, blob, blob_len, now_ms,
        out, sizeof(out));
    err = handle_boot_result(session, result, generation, out, result.out_len,
                             now_ms);
    if (err != CP_SESSION_OK) return err;
    err = flush_tx(session, now_ms);
    refresh_link_stats(session);
    return err != CP_SESSION_OK ? err : map_boot_error(result.error);
}

cp_session_error_t cp_session_set_policy(cp_session_t *session,
                                         cp_boot_consent_fn consent,
                                         cp_boot_usb_active_fn usb_active,
                                         void *context)
{
    cp_boot_error error;

    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    error = cp_bootstrap_set_policy(&session->boot, consent, usb_active,
                                    context);
    if (error != CP_BOOT_OK)
        return CP_SESSION_ERR_ARGUMENT;
    session->policy_consent = consent;
    session->policy_usb_active = usb_active;
    session->policy_context = context;
    if (session->receiver_active) {
        session->receiver_policy_deadline_ms = session->last_now_ms;
        session->receiver_policy_armed = true;
    }
    return CP_SESSION_OK;
}

static bool ap_readback_changed(const cp_session_t *session,
                                   const cp_boot_ap_readback *readback)
{
    const char *link_local;

    if (readback == NULL || !readback->on_air)
        return false;
    if (!session->boot.readback_valid)
        return true;
    if (readback->ssid == NULL ||
        !bounded_text_equal(readback->ssid, session->boot.readback_ssid,
                            CP_WIRE_MAX_SSID))
        return true;
    if (readback->channel != session->boot.readback_channel ||
        readback->security != session->boot.readback_security)
        return true;
    link_local = readback->link_local != NULL ? readback->link_local : "";
    return !bounded_text_equal(link_local, session->boot.readback_link_local,
                               CP_WIRE_MAX_LINK_LOCAL);
}

cp_session_error_t cp_session_set_ap_readback(cp_session_t *session,
                                              const cp_boot_ap_readback *readback,
                                              uint64_t now_ms)
{
    uint8_t out[CP_WIRE_MAX_FRAME];
    uint64_t old_boot_generation;
    cp_boot_result result;
    cp_session_error_t err;

    if (session == NULL || !session->initialized || readback == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;

    old_boot_generation = cp_bootstrap_generation(&session->boot);
    {
        bool changed = ap_readback_changed(session, readback);

        result = cp_bootstrap_set_ap_readback(&session->boot, readback, now_ms,
                                             out, sizeof(out));
        if (result.error == CP_BOOT_OK && readback->on_air && changed)
            session->ap_readback_generation++;
    }
    err = handle_boot_result(session, result, old_boot_generation, out,
                             result.out_len, now_ms);
    if (err != CP_SESSION_OK)
        return err;

    err = flush_tx(session, now_ms);
    if (err != CP_SESSION_OK)
        return err;
    clear_receiver_if_invalid(session);
    refresh_link_stats(session);

    if (result.error == CP_BOOT_ERR_ARGUMENT)
        return CP_SESSION_ERR_ARGUMENT;
    if (result.error == CP_BOOT_ERR_CONFIG)
        return CP_SESSION_ERR_CONFIG;
    return CP_SESSION_OK;
}

static cp_session_error_t receiver_policy_ready(cp_session_t *session,
                                                  uint64_t now_ms)
{
    cp_boot_consent consent;

    if (session->policy_consent == NULL)
        return CP_SESSION_ERR_POLICY;
    consent = session->policy_consent(session->policy_context, now_ms);
    if (consent != CP_BOOT_CONSENT_GRANTED)
        return CP_SESSION_ERR_POLICY;
    if (session->policy_usb_active != NULL &&
        session->policy_usb_active(
            session->policy_context,
            session->boot.have_phone_bluetooth_id
                ? session->boot.phone_bluetooth_id
                : "",
            session->boot.have_phone_usb_id ? session->boot.phone_usb_id
                                            : ""))
        return CP_SESSION_ERR_POLICY;
    if (session->boot.wireless_update_known &&
        !session->boot.wireless_update_available)
        return CP_SESSION_ERR_POLICY;
    return CP_SESSION_OK;
}

cp_session_error_t cp_session_set_receiver_active(cp_session_t *session,
                                                  uint64_t now_ms,
                                                  uint32_t generation,
                                                  const char *peer_id,
                                                  bool active)
{
    bool matches;

    if (session == NULL || !session->initialized || peer_id == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;

    if (generation != cp_link_generation(&session->link) ||
        generation != session->bound_generation) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_GENERATION_MISMATCH);
        return CP_SESSION_ERR_GENERATION_MISMATCH;
    }
    if (cp_link_state(&session->link) != CP_LINK_STATE_NORMAL) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_RECEIVER_STATE);
        return CP_SESSION_ERR_RECEIVER_STATE;
    }
    if (!id_bounded(peer_id)) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_PEER_MISMATCH);
        return CP_SESSION_ERR_PEER_MISMATCH;
    }
    matches = session->boot.have_phone_bluetooth_id &&
              id_equal_ci(peer_id, session->boot.phone_bluetooth_id);
    if (!matches) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_PEER_MISMATCH);
        return CP_SESSION_ERR_PEER_MISMATCH;
    }

    if (!active) {
        if (session->receiver_active)
            clear_receiver(session, true);
        return CP_SESSION_OK;
    }
    if (session->receiver_active &&
        session->receiver_generation == generation) {
        if (!receiver_inputs_valid(session)) {
            clear_receiver(session, true);
            session->stats.receiver_rejections++;
            emit_error(session, CP_SESSION_ERR_RECEIVER_STATE);
            return CP_SESSION_ERR_RECEIVER_STATE;
        }
        session->receiver_policy_deadline_ms =
            add_ms(now_ms, CP_SESSION_POLICY_RECHECK_MS);
        session->receiver_policy_armed = true;
        {
            cp_session_error_t err = receiver_policy_check(session, now_ms);

            if (err != CP_SESSION_OK)
                return err;
        }
        return session->receiver_active ? CP_SESSION_OK
                                        : CP_SESSION_ERR_POLICY;
    }

    if (cp_bootstrap_auth_state(&session->boot) !=
            CP_BOOT_AUTH_SUCCEEDED ||
        cp_bootstrap_state(&session->boot) !=
            CP_BOOT_STATE_AWAITING_RECEIVER ||
        cp_bootstrap_ap_state(&session->boot) != CP_BOOT_AP_READY) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_RECEIVER_STATE);
        return CP_SESSION_ERR_RECEIVER_STATE;
    }
    if (receiver_policy_ready(session, now_ms) != CP_SESSION_OK) {
        session->stats.receiver_rejections++;
        emit_error(session, CP_SESSION_ERR_POLICY);
        return CP_SESSION_ERR_POLICY;
    }
    {
        cp_boot_result hold = cp_bootstrap_hold_offer(
            &session->boot, cp_bootstrap_generation(&session->boot), now_ms);

        if (hold.error != CP_BOOT_OK) {
            session->stats.receiver_rejections++;
            emit_error(session, CP_SESSION_ERR_RECEIVER_STATE);
            return CP_SESSION_ERR_RECEIVER_STATE;
        }
    }

    session->receiver_active = true;
    session->receiver_generation = generation;
    session->receiver_ap_generation = session->ap_readback_generation;
    session->receiver_policy_deadline_ms =
        add_ms(now_ms, CP_SESSION_POLICY_RECHECK_MS);
    session->receiver_policy_armed = true;
    session->last_error = CP_SESSION_OK;
    session->stats.receiver_observations++;
    emit_receiver(session);
    return CP_SESSION_OK;
}

static cp_session_error_t shutdown_session(cp_session_t *session,
                                           cp_session_error_t error,
                                           cp_session_state_t state,
                                           cp_link_reset_reason_t reason,
                                           uint64_t now_ms)
{
    uint32_t old_generation;
    bool was_receiver;
    bool partial = false;
    uint8_t dropped;
    cp_boot_result result;

    old_generation = cp_link_generation(&session->link);
    dropped = tx_drop_all(session, &partial);
    session->stats.csm_stale_replies_dropped += dropped;
    rx_drop_all(session);
    was_receiver = session->receiver_active;
    if (was_receiver)
        clear_receiver(session, false);
    session->receiver_generation = 0u;
    session->last_error = error;

    if (cp_link_state(&session->link) != CP_LINK_STATE_IDLE &&
        cp_link_state(&session->link) != CP_LINK_STATE_DEAD)
        (void)cp_link_cancel(&session->link, reason);
    session->bound_generation = cp_link_generation(&session->link);
    if (session->bound_generation != old_generation)
        session->stats.generation_resets++;

    session->cb_link_reset = false;
    session->cb_link_up = false;
    session->cb_link_state = cp_link_state(&session->link);
    result = cp_bootstrap_transport_reset(&session->boot, now_ms);
    (void)result;
    session->started = false;
    session->state = state;
    session->stats.link_reset_events++;
    emit_event(session, CP_SESSION_EVENT_LINK_RESET, error,
               CP_BOOT_EV_TRANSPORT_RESET, CP_BOOT_OK, reason);
    if (was_receiver)
        emit_receiver(session);
    emit_state(session);
    refresh_link_stats(session);
    return CP_SESSION_OK;
}

cp_session_error_t cp_session_cancel(cp_session_t *session, uint64_t now_ms)
{
    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    return shutdown_session(session, CP_SESSION_ERR_CANCELLED,
                            CP_SESSION_STATE_CANCELLED,
                            CP_LINK_RESET_CANCELLED, now_ms);
}

cp_session_error_t cp_session_transport_reset(cp_session_t *session,
                                              uint64_t now_ms)
{
    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (!clock_accept(session, now_ms))
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    return shutdown_session(session, CP_SESSION_ERR_LINK,
                            CP_SESSION_STATE_DISCONNECTED,
                            CP_LINK_RESET_NONE, now_ms);
}

size_t cp_session_output_peek(const cp_session_t *session,
                              const uint8_t **data)
{
    if (session == NULL || !session->initialized)
        return 0u;
    return cp_link_output_peek(&session->link, data);
}

size_t cp_session_output_pending(const cp_session_t *session)
{
    if (session == NULL || !session->initialized)
        return 0u;
    return cp_link_output_pending(&session->link);
}

cp_session_error_t cp_session_output_consume(cp_session_t *session, size_t len)
{
    int result;

    if (session == NULL || !session->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    session->now_ms = session->last_now_ms;
    result = cp_link_output_consume(&session->link, len);
    if (result != CP_LINK_OK)
        return map_link_result(result);
    result = flush_tx(session, session->now_ms);
    refresh_link_stats(session);
    return (cp_session_error_t)result;
}

size_t cp_session_pending_replies(const cp_session_t *session)
{
    return session == NULL ? 0u : (size_t)session->tx_count;
}

size_t cp_session_pending_rx(const cp_session_t *session)
{
    return session == NULL ? 0u : session->rx_len;
}

cp_session_state_t cp_session_state(const cp_session_t *session)
{
    return session == NULL ? CP_SESSION_STATE_IDLE : session->state;
}

cp_link_state_t cp_session_link_state(const cp_session_t *session)
{
    return session == NULL ? CP_LINK_STATE_IDLE
                           : cp_link_state(&session->link);
}

cp_boot_state cp_session_boot_state(const cp_session_t *session)
{
    return session == NULL ? CP_BOOT_STATE_DISCONNECTED
                           : cp_bootstrap_state(&session->boot);
}

cp_boot_auth_state cp_session_auth_state(const cp_session_t *session)
{
    return session == NULL ? CP_BOOT_AUTH_NONE
                           : cp_bootstrap_auth_state(&session->boot);
}

cp_boot_ap_state cp_session_ap_state(const cp_session_t *session)
{
    return session == NULL ? CP_BOOT_AP_UNKNOWN
                           : cp_bootstrap_ap_state(&session->boot);
}

bool cp_session_start_sent(const cp_session_t *session)
{
    return session != NULL &&
           cp_bootstrap_state(&session->boot) ==
               CP_BOOT_STATE_AWAITING_RECEIVER;
}

bool cp_session_receiver_active(const cp_session_t *session)
{
    return session != NULL && session->receiver_active;
}

uint32_t cp_session_receiver_generation(const cp_session_t *session)
{
    return session == NULL ? 0u : session->receiver_generation;
}

bool cp_session_projection_active(const cp_session_t *session)
{
    (void)session;
    return false;
}

uint32_t cp_session_generation(const cp_session_t *session)
{
    return session == NULL ? 0u : cp_link_generation(&session->link);
}

cp_session_error_t cp_session_last_error(const cp_session_t *session)
{
    return session == NULL ? CP_SESSION_ERR_ARGUMENT : session->last_error;
}

const cp_session_stats_t *cp_session_stats(const cp_session_t *session)
{
    return session == NULL ? NULL : &session->stats;
}
