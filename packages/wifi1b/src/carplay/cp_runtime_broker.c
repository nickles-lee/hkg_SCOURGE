/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_runtime_broker.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define CP_RUNTIME_BROKER_RX_BYTES CP_SPP_SOCKET_MAX_FRAME

static void set_error(cp_runtime_broker *broker, const char *text)
{
    if (broker == NULL || text == NULL) return;
    snprintf(broker->error, sizeof(broker->error), "%s", text);
}

static uint64_t deadline_add(uint64_t now_ms, uint32_t delta_ms)
{
    return now_ms > UINT64_MAX - delta_ms ? UINT64_MAX : now_ms + delta_ms;
}

static bool nonempty(const char *text)
{
    return text != NULL && text[0] != '\0';
}

static bool broker_session_identity_ok(const cp_session_config_t *session)
{
    const cp_boot_config *boot;
    if (session == NULL) return false;
    boot = &session->boot;
    return nonempty(boot->name) &&
           nonempty(boot->model_identifier) &&
           nonempty(boot->manufacturer) &&
           nonempty(boot->serial_number) &&
           nonempty(boot->firmware_version) &&
           nonempty(boot->hardware_version) &&
           nonempty(boot->eap_name) &&
           nonempty(boot->current_language) &&
           boot->supported_languages != NULL &&
           boot->supported_language_count > 0u &&
           nonempty(boot->bluetooth_transport_name) &&
           nonempty(boot->bluetooth_mac) &&
           nonempty(boot->ap_ssid) &&
           nonempty(boot->ap_passphrase) &&
           boot->ap_channel != 0u &&
           nonempty(boot->link_local) &&
           boot->airplay_port != 0u &&
           nonempty(boot->device_identifier);
}

static bool broker_config_valid(const cp_runtime_broker_config_t *config)
{
    if (config == NULL || config->experimental != 1 || config->enabled != 1)
        return false;
    if (config->spp.experimental != 1 || config->auth.experimental != 1)
        return false;
    if (config->role != CP_LINK_ROLE_INITIATOR) return false;
    if (!nonempty(config->spp.bus_address) ||
        !nonempty(config->spp.service_name))
        return false;
    if (!broker_session_identity_ok(&config->session)) return false;
    if (config->consent == NULL || config->usb_active == NULL) return false;
    if (!config->have_ap_readback) return false;
    if (config->ap_readback.on_air &&
        (!nonempty(config->ap_readback.ssid) ||
         config->ap_readback.channel == 0u))
        return false;
    if (config->init_timeout_ms == 0 ||
        config->init_timeout_ms > CP_RUNTIME_BROKER_MAX_INIT_MS)
        return false;
    if (config->drain_timeout_ms == 0 ||
        config->drain_timeout_ms > CP_RUNTIME_BROKER_MAX_DRAIN_MS)
        return false;
    if (config->max_tx_chunks_per_tick > CP_RUNTIME_BROKER_MAX_TX_CHUNKS)
        return false;
    if (config->max_rx_per_tick > CP_RUNTIME_BROKER_MAX_RX_PER_TICK)
        return false;
    if (config->rx != NULL && config->rx->fd < 0) return false;
    return true;
}

static bool policy_allows(cp_runtime_broker *broker, uint64_t now_ms)
{
    cp_boot_consent consent;
    if (broker == NULL || broker->cfg.consent == NULL ||
        broker->cfg.usb_active == NULL)
        return false;
    consent = broker->cfg.consent(broker->cfg.policy_context, now_ms);
    if (consent == CP_BOOT_CONSENT_DENIED) return false;
    if (broker->cfg.usb_active(broker->cfg.policy_context,
                               broker->bound_peer, ""))
        return false;
    return true;
}

static void clear_peer(cp_runtime_broker *broker)
{
    broker->peer[0] = '\0';
    broker->bound_peer[0] = '\0';
}

static void cancel_internal(cp_runtime_broker *broker, uint64_t now_ms,
                            bool close_spp, bool terminal, bool policy,
                            bool auth, bool session, cp_runtime_broker_state_t state,
                            const char *why)
{
    if (broker == NULL) return;
    if (broker->auth_inited) {
        cp_auth_broker_destroy(&broker->auth, now_ms);
        broker->auth_inited = false;
    }
    if (broker->session_inited) {
        (void)cp_session_cancel(&broker->session, now_ms);
        broker->session_inited = false;
    }
    broker->init_deadline_ms = 0;
    broker->draining = false;
    broker->drain_quiescent = false;
    broker->spp_connected = false;
    broker->terminal_gate = terminal;
    broker->policy_blocked = policy;
    broker->auth_failed = auth;
    broker->session_failed = session;
    broker->restart_required = true;
    broker->state = state;
    broker->fatal = (state == CP_RUNTIME_BROKER_FATAL);
    set_error(broker, why);
    if (close_spp && broker->spp != NULL) {
        cp_spp_dbus_close(broker->spp);
        broker->spp = NULL;
    }
}

/* Keep one coherent total-init gate for the tick and every trusted external
 * input path.  The deadline is armed before bus setup and is never refreshed. */
static bool init_deadline_expired(cp_runtime_broker *broker, uint64_t now_ms)
{
    if (broker == NULL || broker->init_deadline_ms == 0 ||
        now_ms < broker->init_deadline_ms)
        return false;
    broker->init_timed_out = true;
    broker->stats.init_timeouts++;
    cancel_internal(broker, now_ms, true, true, false, false, false,
                    CP_RUNTIME_BROKER_FATAL, "init_timeout");
    return true;
}

/* The callbacks object must have static lifetime. */
static void on_spp_state(void *user, cp_spp_dbus_state old_state,
                         cp_spp_dbus_state new_state);
static void on_spp_validated(void *user);
static void on_spp_status(void *user, const char *object_path,
                          const char *status);
static void on_spp_removed(void *user, const char *object_path);
static void on_spp_fatal(void *user, const char *reason);
static void on_spp_rx(void *user, const char *peer, uint8_t uuid_kind,
                      const uint8_t *data, size_t length);
static void on_spp_stub_ack(void *user, size_t length);

static const cp_spp_dbus_callbacks g_spp_callbacks = {
    .on_state = on_spp_state,
    .on_validated = on_spp_validated,
    .on_status = on_spp_status,
    .on_removed = on_spp_removed,
    .on_fatal = on_spp_fatal,
    .on_rx = on_spp_rx,
    .on_stub_ack = on_spp_stub_ack
};

static bool open_spp_real(cp_runtime_broker *broker, uint64_t now_ms)
{
    char error[256] = "";
    if (broker->spp != NULL) return true;
    int rc = cp_spp_dbus_open(&broker->spp, &broker->spp_storage,
                              &broker->cfg.spp, &g_spp_callbacks, broker,
                              error, sizeof(error));
    if (rc != CP_SPP_DBUS_OK) {
        set_error(broker, error[0] ? error : "spp open failed");
        broker->spp = NULL;
        return false;
    }
    rc = cp_spp_dbus_start(broker->spp, now_ms);
    if (rc != CP_SPP_DBUS_OK) {
        cp_spp_dbus_close(broker->spp);
        broker->spp = NULL;
        set_error(broker, "spp start failed");
        return false;
    }
    return true;
}

static uint64_t drain_deadline_for(const cp_runtime_broker *broker,
                                   uint64_t now_ms)
{
    uint64_t deadline = deadline_add(now_ms, broker->cfg.drain_timeout_ms);
    if (broker->init_deadline_ms != 0 &&
        broker->init_deadline_ms < deadline)
        deadline = broker->init_deadline_ms;
    return deadline;
}

static void clear_early_rx(cp_runtime_broker *broker)
{
    broker->early_count = 0;
    broker->early_replayed = 0;
    for (size_t i = 0; i < CP_RUNTIME_BROKER_MAX_EARLY_RX; i++) {
        memset(&broker->early[i], 0, sizeof(broker->early[i]));
    }
}

static bool store_early_rx(cp_runtime_broker *broker, const cp_spp_packet *packet)
{
    if (broker->early_count >= CP_RUNTIME_BROKER_MAX_EARLY_RX)
        return false;
    cp_runtime_broker_early_rx_t *slot = &broker->early[broker->early_count];
    if (packet->payload_length > sizeof(slot->bytes)) return false;
    snprintf(slot->peer, sizeof(slot->peer), "%s", packet->peer);
    slot->uuid_kind = packet->uuid_kind;
    slot->length = (uint16_t)packet->payload_length;
    memcpy(slot->bytes, packet->payload, packet->payload_length);
    broker->early_count++;
    broker->stats.early_rx++;
    return true;
}

static void begin_drain(cp_runtime_broker *broker, uint64_t now_ms,
                        const char *peer)
{
    broker->draining = true;
    broker->drain_quiescent = false;
    broker->drain_deadline_ms = peer != NULL ?
        drain_deadline_for(broker, now_ms) : 0;
    if (peer != NULL) broker->state = CP_RUNTIME_BROKER_DRAINING;
    broker->stats.drains++;
    if (peer != NULL)
        snprintf(broker->peer, sizeof(broker->peer), "%s", peer);
    if (broker->rx == NULL) broker->drain_quiescent = true;
}

static bool drain_old_rx(cp_runtime_broker *broker, uint64_t now_ms)
{
    uint8_t buffer[CP_RUNTIME_BROKER_RX_BYTES];
    cp_spp_packet packet;
    unsigned limit = broker->cfg.max_rx_per_tick ?
        broker->cfg.max_rx_per_tick : 8u;
    if (broker->rx == NULL) {
        broker->drain_quiescent = true;
        return true;
    }
    if (broker->rx->fd < 0) {
        broker->stats.drain_timeouts++;
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "rx_closed");
        return false;
    }
    if (broker->drain_deadline_ms != 0 &&
        now_ms >= broker->drain_deadline_ms) {
        broker->stats.drain_timeouts++;
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "stale_rx_drain_timeout");
        return false;
    }
    for (unsigned i = 0; i < limit; i++) {
        int rc = cp_spp_socket_receive(broker->rx, buffer, sizeof(buffer),
                                       &packet);
        if (rc == 0) {
            broker->drain_quiescent = true;
            return true;
        }
        if (rc > 0) {
            if (broker->preserve_early_rx && broker->spp != NULL &&
                cp_spp_dbus_attached(broker->spp) &&
                store_early_rx(broker, &packet))
                continue;
            broker->stats.rx_dropped++;
            continue;
        }
        if (errno == EBADF || errno == ENOTSOCK) {
            broker->stats.drain_timeouts++;
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "rx_drain_io_error");
            return false;
        }
        broker->stats.rx_rejected++;
    }
    if (broker->drain_quiescent) return true;
    if (broker->drain_deadline_ms != 0 &&
        now_ms >= broker->drain_deadline_ms) {
        broker->stats.drain_timeouts++;
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "stale_rx_drain_timeout");
        return false;
    }
    return true;
}

static cp_session_error_t start_session(cp_runtime_broker *broker,
                                        uint64_t now_ms)
{
    cp_session_error_t error;
    if (broker->session_inited) return CP_SESSION_OK;
    error = cp_session_init(&broker->session, &broker->cfg.session, NULL,
                            broker, now_ms);
    if (error != CP_SESSION_OK) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "session_init_failed");
        return error;
    }
    broker->session_inited = true;
    error = cp_session_set_policy(&broker->session, broker->cfg.consent,
        broker->cfg.usb_active, broker->cfg.policy_context);
    if (error != CP_SESSION_OK) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "session_policy_failed");
        return error;
    }
    if (broker->cfg.have_ap_readback) {
        error = cp_session_set_ap_readback(&broker->session,
            &broker->cfg.ap_readback, now_ms);
        if (error != CP_SESSION_OK) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "session_ap_failed");
            return error;
        }
    }
    error = cp_auth_broker_init(&broker->auth, &broker->session,
                                &broker->cfg.auth);
    if (error != CP_SESSION_OK) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "auth_broker_init_failed");
        return error;
    }
    broker->auth_inited = true;
    error = cp_session_start(&broker->session, broker->cfg.role, now_ms);
    if (error != CP_SESSION_OK) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "session_start_failed");
        return error;
    }
    broker->state = CP_RUNTIME_BROKER_STARTING;
    broker->stats.sessions++;
    return CP_SESSION_OK;
}

static void on_spp_state(void *user, cp_spp_dbus_state old_state,
                         cp_spp_dbus_state new_state)
{
    cp_runtime_broker *broker = user;
    if (broker == NULL) return;
    if (old_state == CP_SPP_DBUS_STATE_CONNECTED &&
        new_state != CP_SPP_DBUS_STATE_CONNECTED) {
        broker->spp_connected = false;
        broker->stats.spp_disconnects++;
    }
    if (new_state == CP_SPP_DBUS_STATE_FATAL) {
        broker->terminal_gate = true;
        broker->fatal = true;
        broker->state = CP_RUNTIME_BROKER_FATAL;
    }
}

static void on_spp_validated(void *user)
{
    (void)user;
}

static void on_spp_status(void *user, const char *object_path,
                          const char *status)
{
    (void)user;
    (void)object_path;
    (void)status;
}

static void on_spp_removed(void *user, const char *object_path)
{
    cp_runtime_broker *broker = user;
    (void)object_path;
    if (broker == NULL) return;
    broker->spp_connected = false;
    broker->stats.spp_disconnects++;
}

static void on_spp_fatal(void *user, const char *reason)
{
    cp_runtime_broker *broker = user;
    if (broker == NULL) return;
    broker->terminal_gate = true;
    broker->fatal = true;
    broker->state = CP_RUNTIME_BROKER_FATAL;
    set_error(broker, reason != NULL ? reason : "spp_fatal");
}

static void on_spp_rx(void *user, const char *peer, uint8_t uuid_kind,
                      const uint8_t *data, size_t length)
{
    cp_runtime_broker *broker = user;
    (void)peer;
    (void)uuid_kind;
    if (broker == NULL || !broker->session_inited ||
        broker->session_failed ||
        broker->state == CP_RUNTIME_BROKER_BLOCKED ||
        broker->state == CP_RUNTIME_BROKER_FATAL ||
        broker->draining) {
        if (broker != NULL) broker->stats.rx_dropped++;
        return;
    }
    if (cp_session_feed(&broker->session, data, length,
                        broker->last_now_ms) != CP_SESSION_OK) {
        /* Promote immediately, but never close or re-enter the SPP driver
         * from this callback.  The tick/feed path cancels exactly once after
         * deliver_rx() returns and before any new RX or TX. */
        broker->session_failed = true;
        broker->terminal_gate = true;
        broker->fatal = true;
        broker->state = CP_RUNTIME_BROKER_FATAL;
        broker->stats.rx_dropped++;
        set_error(broker, "rx_session_feed_failed");
    }
}

static void on_spp_stub_ack(void *user, size_t length)
{
    cp_runtime_broker *broker = user;
    (void)length;
    if (broker != NULL) broker->stats.stub_acks++;
}

static bool drain_active_rx(cp_runtime_broker *broker, uint64_t now_ms)
{
    uint8_t buffer[CP_RUNTIME_BROKER_RX_BYTES];
    cp_spp_packet packet;
    unsigned limit = broker->cfg.max_rx_per_tick ?
        broker->cfg.max_rx_per_tick : 8u;
    if (broker->rx == NULL || !broker->spp_connected ||
        !broker->session_inited || broker->session_failed ||
        broker->terminal_gate || broker->fatal)
        return true;
    if (broker->rx->fd < 0) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, "rx_closed");
        return false;
    }
    for (unsigned i = 0; i < limit; i++) {
        int rc = cp_spp_socket_receive(broker->rx, buffer, sizeof(buffer),
                                       &packet);
        if (rc == 0) return true;
        if (rc < 0) {
            if (errno == EBADF || errno == ENOTSOCK) {
                cancel_internal(broker, now_ms, true, true, false, false,
                                false, CP_RUNTIME_BROKER_FATAL,
                                "rx_io_error");
                return false;
            }
            broker->stats.rx_rejected++;
            continue;
        }
        uint32_t dropped = broker->stats.rx_dropped;
        int delivered = cp_spp_dbus_deliver_rx(broker->spp, packet.peer,
            packet.uuid_kind, packet.payload, packet.payload_length);
        if (broker->session_failed || broker->fatal) {
            /* The callback promoted FATAL without touching the SPP driver.
             * We are back outside it, so cancel/close exactly once before any
             * further delivery or TX. */
            char why[sizeof(broker->error)];
            snprintf(why, sizeof(why), "%s",
                     broker->error[0] ? broker->error :
                                        "rx_session_feed_failed");
            cancel_internal(broker, now_ms, true, true, false, false, true,
                            CP_RUNTIME_BROKER_FATAL, why);
            return false;
        }
        if (delivered == CP_SPP_DBUS_ERR_REJECTED) {
            broker->stats.rx_rejected++;
        } else if (delivered == CP_SPP_DBUS_ERR_NOT_CONNECTED) {
            broker->spp_connected = false;
            broker->stats.spp_disconnects++;
            cancel_internal(broker, now_ms, true, false, false, false, false,
                            CP_RUNTIME_BROKER_BLOCKED, "spp_disconnected");
            return false;
        } else if (delivered != CP_SPP_DBUS_OK) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "rx_delivery_failed");
            return false;
        } else if (broker->stats.rx_dropped == dropped) {
            broker->stats.rx_packets++;
        }
    }
    return true;
}

static bool replay_early_rx(cp_runtime_broker *broker, uint64_t now_ms)
{
    while (broker->early_replayed < broker->early_count) {
        cp_runtime_broker_early_rx_t *slot =
            &broker->early[broker->early_replayed];
        uint32_t dropped = broker->stats.rx_dropped;
        int delivered = cp_spp_dbus_deliver_rx(broker->spp, slot->peer,
            slot->uuid_kind, slot->bytes, slot->length);
        if (broker->session_failed || broker->fatal) {
            char why[sizeof(broker->error)];
            snprintf(why, sizeof(why), "%s",
                     broker->error[0] ? broker->error :
                                        "rx_session_feed_failed");
            cancel_internal(broker, now_ms, true, true, false, false, true,
                            CP_RUNTIME_BROKER_FATAL, why);
            clear_early_rx(broker);
            return false;
        }
        broker->early_replayed++;
        if (delivered == CP_SPP_DBUS_OK) {
            if (broker->stats.rx_dropped == dropped)
                broker->stats.rx_packets++;
        } else if (delivered == CP_SPP_DBUS_ERR_REJECTED) {
            broker->stats.rx_rejected++;
        } else if (delivered == CP_SPP_DBUS_ERR_NOT_CONNECTED) {
            broker->spp_connected = false;
            broker->stats.spp_disconnects++;
            cancel_internal(broker, now_ms, true, false, false, false, false,
                            CP_RUNTIME_BROKER_BLOCKED, "spp_disconnected");
            clear_early_rx(broker);
            return false;
        } else {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "rx_delivery_failed");
            clear_early_rx(broker);
            return false;
        }
    }
    clear_early_rx(broker);
    return true;
}

static bool pump_tx(cp_runtime_broker *broker, uint64_t now_ms)
{
    unsigned limit = broker->cfg.max_tx_chunks_per_tick ?
        broker->cfg.max_tx_chunks_per_tick : 2u;
    if (!broker->session_inited || !broker->spp_connected ||
        broker->session_failed || broker->terminal_gate || broker->fatal ||
        broker->state == CP_RUNTIME_BROKER_BLOCKED ||
        broker->state == CP_RUNTIME_BROKER_FATAL ||
        broker->draining)
        return true;
    for (unsigned i = 0; i < limit; i++) {
        size_t pending = cp_session_output_pending(&broker->session);
        if (pending == 0) return true;
        if (pending > CP_SPP_DBUS_MAX_TX_BYTES)
            pending = CP_SPP_DBUS_MAX_TX_BYTES;
        const uint8_t *data = NULL;
        size_t available = cp_session_output_peek(&broker->session, &data);
        if (available == 0 || data == NULL) return true;
        if (pending > available) pending = available;
        int rc = cp_spp_dbus_send(broker->spp, data, pending, now_ms);
        if (rc == CP_SPP_DBUS_ERR_BACKPRESSURE) {
            broker->stats.tx_backpressure++;
            return true;
        }
        if (rc == CP_SPP_DBUS_ERR_NOT_CONNECTED) {
            broker->spp_connected = false;
            broker->stats.spp_disconnects++;
            cancel_internal(broker, now_ms, true, false, false, false, false,
                            CP_RUNTIME_BROKER_BLOCKED, "spp_disconnected");
            return false;
        }
        if (rc != CP_SPP_DBUS_OK) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "spp_send_failed");
            return false;
        }
        cp_session_error_t error =
            cp_session_output_consume(&broker->session, pending);
        if (error != CP_SESSION_OK) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "session_output_consume_failed");
            return false;
        }
        broker->stats.tx_chunks++;
        broker->stats.tx_bytes += (uint32_t)pending;
    }
    return true;
}

cp_session_error_t cp_runtime_broker_init(
    cp_runtime_broker *broker, const cp_runtime_broker_config_t *config)
{
    if (broker == NULL) return CP_SESSION_ERR_ARGUMENT;
    memset(broker, 0, sizeof(*broker));
    if (!broker_config_valid(config)) {
        set_error(broker, "runtime configuration rejected");
        return config != NULL && (config->experimental != 1 ||
                                  config->enabled != 1) ?
               CP_SESSION_ERR_PROVIDER : CP_SESSION_ERR_CONFIG;
    }
    broker->cfg = *config;
    broker->rx = config->rx;
    broker->state = CP_RUNTIME_BROKER_IDLE;
    broker->initialized = true;
    return CP_SESSION_OK;
}

cp_session_error_t cp_runtime_broker_start(cp_runtime_broker *broker,
                                           uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (now_ms < broker->last_now_ms)
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (broker->started || broker->spp != NULL)
        return CP_SESSION_ERR_STATE;
    if (!policy_allows(broker, now_ms)) {
        broker->policy_blocked = true;
        broker->state = CP_RUNTIME_BROKER_BLOCKED;
        set_error(broker, "policy or consent blocks runtime start");
        return CP_SESSION_ERR_POLICY;
    }
    broker->last_now_ms = now_ms;
    broker->init_deadline_ms = deadline_add(now_ms,
                                            broker->cfg.init_timeout_ms);
    broker->preserve_early_rx = true;
    broker->spp_connect_generation = 0u;
    clear_early_rx(broker);
    if (!open_spp_real(broker, now_ms)) return CP_SESSION_ERR_LINK;
    if (broker->fatal) {
        if (broker->spp != NULL) {
            cp_spp_dbus_close(broker->spp);
            broker->spp = NULL;
        }
        set_error(broker, "spp fatal during start");
        return CP_SESSION_ERR_LINK;
    }
    broker->started = true;
    broker->policy_blocked = false;
    broker->restart_required = false;
    broker->terminal_gate = false;
    broker->auth_failed = false;
    broker->session_failed = false;
    broker->init_timed_out = false;
    broker->fatal = false;
    broker->state = CP_RUNTIME_BROKER_WAIT_SPP;
    begin_drain(broker, now_ms, NULL);
    return CP_SESSION_OK;
}

cp_session_error_t cp_runtime_broker_restart(cp_runtime_broker *broker,
                                             uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (now_ms < broker->last_now_ms)
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (!policy_allows(broker, now_ms)) {
        broker->policy_blocked = true;
        cancel_internal(broker, now_ms, true, true, true, false, false,
                        CP_RUNTIME_BROKER_BLOCKED,
                        "policy or consent blocks runtime restart");
        return CP_SESSION_ERR_POLICY;
    }
    cancel_internal(broker, now_ms, true, false, false, false, false,
                    CP_RUNTIME_BROKER_IDLE, "restarting");
    broker->started = false;
    broker->restart_required = false;
    broker->terminal_gate = false;
    broker->auth_failed = false;
    broker->session_failed = false;
    broker->init_timed_out = false;
    broker->policy_blocked = false;
    broker->fatal = false;
    broker->last_now_ms = now_ms;
    broker->init_deadline_ms = deadline_add(now_ms,
                                            broker->cfg.init_timeout_ms);
    /* Drop bytes that belong to the connection being replaced, then keep
     * preserving pre-session frames for the new epoch.  A frame that arrives
     * after this restart but before the session starts must not be lost. */
    clear_early_rx(broker);
    broker->preserve_early_rx = true;
    broker->spp_connect_generation = 0u;
    clear_peer(broker);
    if (!open_spp_real(broker, now_ms)) return CP_SESSION_ERR_LINK;
    if (broker->fatal) {
        if (broker->spp != NULL) {
            cp_spp_dbus_close(broker->spp);
            broker->spp = NULL;
        }
        set_error(broker, "spp fatal during restart");
        return CP_SESSION_ERR_LINK;
    }
    broker->started = true;
    broker->state = CP_RUNTIME_BROKER_WAIT_SPP;
    begin_drain(broker, now_ms, NULL);
    return CP_SESSION_OK;
}

size_t cp_runtime_broker_pollfds(cp_runtime_broker *broker,
                                 struct pollfd *pfds, size_t capacity)
{
    size_t count = 0;
    if (broker == NULL || pfds == NULL || capacity == 0) return 0;
    if (broker->spp != NULL) {
        struct pollfd pfd;
        if (cp_spp_dbus_pollfd(broker->spp, &pfd) > 0 && pfd.fd >= 0) {
            pfds[count++] = pfd;
        }
    }
    if (broker->rx != NULL && broker->rx->fd >= 0 && count < capacity) {
        pfds[count].fd = broker->rx->fd;
        pfds[count].events = POLLIN;
        pfds[count].revents = 0;
        count++;
    }
    if (broker->auth_inited && count < capacity) {
        struct pollfd pfd;
        if (cp_auth_broker_pollfd(&broker->auth, &pfd) > 0 && pfd.fd >= 0) {
            bool duplicate = false;
            for (size_t i = 0; i < count; i++)
                if (pfds[i].fd == pfd.fd) duplicate = true;
            if (!duplicate) pfds[count++] = pfd;
        }
    }
    return count;
}

static short helper_revents_for(cp_runtime_broker *broker,
                                const struct pollfd *pfds, size_t nfds)
{
    struct pollfd helper;
    if (!broker->auth_inited) return 0;
    if (cp_auth_broker_pollfd(&broker->auth, &helper) <= 0 ||
        helper.fd < 0)
        return 0;
    for (size_t i = 0; i < nfds; i++)
        if (pfds[i].fd == helper.fd) return pfds[i].revents;
    return 0;
}

cp_session_error_t cp_runtime_broker_tick(cp_runtime_broker *broker,
                                          const struct pollfd *pfds,
                                          size_t nfds, uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (nfds > CP_RUNTIME_BROKER_MAX_POLLFDS ||
        (nfds != 0 && pfds == NULL))
        return CP_SESSION_ERR_ARGUMENT;
    if (!broker->started) return CP_SESSION_ERR_STATE;
    if (now_ms < broker->last_now_ms) {
        cancel_internal(broker, broker->last_now_ms, true, true, false, false,
                        false, CP_RUNTIME_BROKER_FATAL, "clock_rollback");
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    }
    broker->last_now_ms = now_ms;
    if (broker->fatal) {
        if (broker->restart_required &&
            broker->state == CP_RUNTIME_BROKER_FATAL &&
            broker->spp == NULL)
            return CP_SESSION_ERR_STATE;
        char why[sizeof(broker->error)];
        snprintf(why, sizeof(why), "%s",
                 broker->error[0] ? broker->error : "fatal");
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL, why);
        return CP_SESSION_ERR_LINK;
    }
    if (broker->session_failed) {
        char why[sizeof(broker->error)];
        snprintf(why, sizeof(why), "%s",
                 broker->error[0] ? broker->error :
                                    "rx_session_feed_failed");
        cancel_internal(broker, now_ms, true, true, false, false, true,
                        CP_RUNTIME_BROKER_FATAL, why);
        return CP_SESSION_ERR_LINK;
    }
    if (broker->restart_required)
        return CP_SESSION_ERR_STATE;

    if (!policy_allows(broker, now_ms)) {
        broker->policy_blocked = true;
        broker->stats.policy_cancels++;
        cancel_internal(broker, now_ms, true, true, true, false, false,
                        CP_RUNTIME_BROKER_BLOCKED, "policy_cancel");
        return CP_SESSION_ERR_POLICY;
    }
    if (init_deadline_expired(broker, now_ms))
        return CP_SESSION_ERR_TIMEOUT;

    short helper_events = helper_revents_for(broker, pfds, nfds);
    if (broker->auth_inited) {
        /* cp_auth_broker_service() ticks the bound session first, so the
         * runtime must not tick it a second time. */
        cp_session_error_t error =
            cp_auth_broker_service(&broker->auth, helper_events, now_ms);
        if (error == CP_SESSION_ERR_CLOCK_ROLLBACK) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "clock_rollback");
            return error;
        }
        if (error == CP_SESSION_ERR_POLICY) {
            broker->policy_blocked = true;
            cancel_internal(broker, now_ms, true, true, true, false, false,
                            CP_RUNTIME_BROKER_BLOCKED, "session_policy");
            return error;
        }
        if (error != CP_SESSION_OK) {
            char why[96];
            bool auth_failure = error == CP_SESSION_ERR_AUTH_FAILED ||
                                error == CP_SESSION_ERR_PROVIDER ||
                                error == CP_SESSION_ERR_IDENTIFICATION_REJECTED;
            snprintf(why, sizeof(why), "auth_service_%d_sess_%d_auth_%d",
                     (int)error,
                     (int)cp_session_last_error(&broker->session),
                     (int)cp_session_auth_state(&broker->session));
            cancel_internal(broker, now_ms, true, true, false, auth_failure,
                            !auth_failure, CP_RUNTIME_BROKER_FATAL, why);
            return error;
        }
    }
    if (broker->spp != NULL) {
        int rc = cp_spp_dbus_tick(broker->spp, now_ms);
        if (broker->session_failed || broker->fatal) {
            /* on_spp_rx only promoted FATAL; it did not close or re-enter the
             * SPP driver.  We are outside the callback, so close exactly once
             * before any further RX, auth service or TX. */
            char why[sizeof(broker->error)];
            snprintf(why, sizeof(why), "%s",
                     broker->error[0] ? broker->error :
                                        "rx_session_feed_failed");
            cancel_internal(broker, now_ms, true, true, false, false,
                            broker->session_failed, CP_RUNTIME_BROKER_FATAL,
                            why);
            return CP_SESSION_ERR_LINK;
        }
        if (rc != CP_SPP_DBUS_OK) {
            if (cp_spp_dbus_state_get(broker->spp) ==
                CP_SPP_DBUS_STATE_FATAL) {
                const char *why = cp_spp_dbus_error(broker->spp);
                cancel_internal(broker, now_ms, true, true, false, false,
                                false, CP_RUNTIME_BROKER_FATAL,
                                why && *why ? why : "spp_fatal");
                return CP_SESSION_ERR_LINK;
            }
            return CP_SESSION_ERR_LINK;
        }
    }
    if (broker->session_inited) {
        if (cp_session_auth_state(&broker->session) == CP_BOOT_AUTH_FAILED) {
            broker->auth_failed = true;
            broker->stats.auth_failures++;
            cancel_internal(broker, now_ms, true, true, false, true, false,
                            CP_RUNTIME_BROKER_FATAL, "auth_failed");
            return CP_SESSION_ERR_AUTH_FAILED;
        }
        cp_session_state_t state = cp_session_state(&broker->session);
        if (state == CP_SESSION_STATE_FAILED ||
            state == CP_SESSION_STATE_CANCELLED) {
            cp_session_error_t error = cp_session_last_error(&broker->session);
            if (error == CP_SESSION_ERR_POLICY) {
                broker->policy_blocked = true;
                cancel_internal(broker, now_ms, true, true, true, false,
                                false, CP_RUNTIME_BROKER_BLOCKED,
                                "session_policy");
                return error;
            }
            broker->session_failed = true;
            broker->stats.session_failures++;
            cancel_internal(broker, now_ms, true, true, false, false, true,
                            CP_RUNTIME_BROKER_FATAL, "session_failed");
            return error != CP_SESSION_OK ? error : CP_SESSION_ERR_STATE;
        }
        if (cp_session_auth_state(&broker->session) ==
                CP_BOOT_AUTH_SUCCEEDED) {
            /* A late AA05 must not revive or extend an expired total init
             * deadline.  The top-of-tick gate should already have caught it;
             * this guard keeps the deadline from being cleared if state changed
             * later in this tick. */
            if (init_deadline_expired(broker, now_ms))
                return CP_SESSION_ERR_TIMEOUT;
            broker->init_deadline_ms = 0;
            if (broker->state == CP_RUNTIME_BROKER_STARTING)
                broker->state = CP_RUNTIME_BROKER_ACTIVE;
        }
    }

    if (broker->spp == NULL) {
        broker->state = broker->restart_required ?
            CP_RUNTIME_BROKER_BLOCKED : CP_RUNTIME_BROKER_WAIT_SPP;
        return CP_SESSION_ERR_STATE;
    }

    cp_spp_dbus_state spp_state = cp_spp_dbus_state_get(broker->spp);
    if (spp_state == CP_SPP_DBUS_STATE_FATAL) {
        const char *why = cp_spp_dbus_error(broker->spp);
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL,
                        why && *why ? why : "spp_fatal");
        return CP_SESSION_ERR_LINK;
    }
    if (spp_state == CP_SPP_DBUS_STATE_LISTENING &&
        cp_spp_dbus_listener_owned(broker->spp) == 0) {
        int rc = cp_spp_dbus_listen(broker->spp, now_ms);
        if (rc != CP_SPP_DBUS_OK) {
            if (cp_spp_dbus_state_get(broker->spp) ==
                CP_SPP_DBUS_STATE_FATAL) {
                const char *why = cp_spp_dbus_error(broker->spp);
                cancel_internal(broker, now_ms, true, true, false, false,
                                false, CP_RUNTIME_BROKER_FATAL,
                                why && *why ? why : "spp_listen_failed");
                return CP_SESSION_ERR_LINK;
            }
        }
    }
    spp_state = cp_spp_dbus_state_get(broker->spp);
    if (spp_state == CP_SPP_DBUS_STATE_FATAL) {
        const char *why = cp_spp_dbus_error(broker->spp);
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_FATAL,
                        why && *why ? why : "spp_fatal");
        return CP_SESSION_ERR_LINK;
    }
    if (spp_state != CP_SPP_DBUS_STATE_CONNECTED) {
        /* No early buffer may survive the connection it was read from. */
        clear_early_rx(broker);
        if (broker->session_inited) {
            broker->stats.spp_disconnects++;
            cancel_internal(broker, now_ms, false, false, false, false, false,
                            CP_RUNTIME_BROKER_WAIT_SPP, "spp_disconnected");
            /* The driver keeps the listener; a later connected confirmation
             * advances its connection generation and starts a clean epoch. */
            broker->restart_required = false;
            broker->terminal_gate = false;
            broker->fatal = false;
            broker->policy_blocked = false;
            broker->auth_failed = false;
            broker->session_failed = false;
            clear_peer(broker);
            broker->draining = false;
        } else {
            broker->state = broker->restart_required ?
                CP_RUNTIME_BROKER_BLOCKED : CP_RUNTIME_BROKER_WAIT_SPP;
        }
        return CP_SESSION_OK;
    }

    char peer[18] = "";
    if (cp_spp_dbus_peer(broker->spp, peer, sizeof(peer)) != CP_SPP_DBUS_OK) {
        broker->stats.rx_rejected++;
        return CP_SESSION_OK;
    }
    uint32_t connect_generation =
        cp_spp_dbus_connect_generation(broker->spp);
    if (connect_generation != 0u &&
        broker->spp_connect_generation != 0u &&
        connect_generation != broker->spp_connect_generation &&
        broker->session_inited) {
        /* A fresh connected confirmation invalidates every per-connection
         * byte/sequence/auth fact, even when the peer MAC is unchanged. */
        uint64_t init_deadline = broker->init_deadline_ms;
        cancel_internal(broker, now_ms, false, false, false, false, false,
                        CP_RUNTIME_BROKER_IDLE, "spp_reconnected");
        clear_early_rx(broker);
        broker->init_deadline_ms = init_deadline;
        broker->restart_required = false;
        broker->terminal_gate = false;
        broker->policy_blocked = false;
        broker->auth_failed = false;
        broker->session_failed = false;
        broker->fatal = false;
        clear_peer(broker);
        broker->draining = false;
        broker->stats.spp_reconnects++;
    }
    if (connect_generation != 0u)
        broker->spp_connect_generation = connect_generation;
    if (!broker->spp_connected) {
        broker->spp_connected = true;
        broker->stats.spp_connects++;
    }
    if (!broker->draining && broker->bound_peer[0] != '\0' &&
        strcmp(peer, broker->bound_peer) != 0) {
        cancel_internal(broker, now_ms, true, true, false, false, false,
                        CP_RUNTIME_BROKER_BLOCKED, "peer_changed");
        return CP_SESSION_OK;
    }
    if (broker->draining || strcmp(peer, broker->bound_peer) != 0) {
        if (!broker->draining)
            begin_drain(broker, now_ms, peer);
        else if (broker->drain_deadline_ms == 0)
            broker->drain_deadline_ms =
                drain_deadline_for(broker, now_ms);
        if (!drain_old_rx(broker, now_ms))
            return CP_SESSION_ERR_LINK;
        if (!broker->drain_quiescent) return CP_SESSION_OK;
        broker->draining = false;
        if (broker->epoch == UINT32_MAX) {
            cancel_internal(broker, now_ms, true, true, false, false, false,
                            CP_RUNTIME_BROKER_FATAL, "epoch_exhausted");
            return CP_SESSION_ERR_STATE;
        }
        broker->epoch++;
        broker->stats.epochs++;
        snprintf(broker->bound_peer, sizeof(broker->bound_peer), "%s", peer);
        broker->state = CP_RUNTIME_BROKER_STARTING;
    }
    if (!broker->session_inited) {
        cp_session_error_t error = start_session(broker, now_ms);
        if (error != CP_SESSION_OK) return error;
    }
    if (!replay_early_rx(broker, now_ms)) return CP_SESSION_ERR_LINK;
    if (!drain_active_rx(broker, now_ms)) return CP_SESSION_ERR_LINK;
    if (!pump_tx(broker, now_ms)) return CP_SESSION_ERR_LINK;
    if (broker->session_failed) {
        cancel_internal(broker, now_ms, true, true, false, false, true,
                        CP_RUNTIME_BROKER_FATAL, "rx_session_failed");
        return CP_SESSION_ERR_LINK;
    }
    if (broker->auth_failed) return CP_SESSION_ERR_AUTH_FAILED;
    return CP_SESSION_OK;
}


uint64_t cp_runtime_broker_next_deadline(const cp_runtime_broker *broker)
{
    uint64_t deadline = UINT64_MAX;
    if (broker == NULL || !broker->started) return UINT64_MAX;
    if (broker->session_inited) {
        uint64_t d = cp_session_next_deadline(&broker->session);
        if (d < deadline) deadline = d;
    }
    if (broker->auth_inited) {
        uint64_t d = cp_auth_broker_next_deadline(&broker->auth);
        if (d < deadline) deadline = d;
    }
    if (broker->spp != NULL) {
        uint64_t d = UINT64_MAX;
        if (cp_spp_dbus_next_deadline(broker->spp, &d) > 0 && d < deadline)
            deadline = d;
    }
    if (broker->init_deadline_ms != 0 &&
        broker->init_deadline_ms < deadline)
        deadline = broker->init_deadline_ms;
    if (broker->draining && broker->drain_deadline_ms != 0 &&
        broker->drain_deadline_ms < deadline)
        deadline = broker->drain_deadline_ms;
    return deadline;
}

cp_session_error_t cp_runtime_broker_set_ap_readback(
    cp_runtime_broker *broker, const cp_boot_ap_readback *readback,
    uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized || readback == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    if (broker->terminal_gate || broker->session_failed || broker->fatal)
        return CP_SESSION_ERR_STATE;
    if (now_ms < broker->last_now_ms)
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (init_deadline_expired(broker, now_ms))
        return CP_SESSION_ERR_TIMEOUT;
    /* Only an on-air readback needs an SSID/channel; an off-air readback is
     * an explicit trusted withdrawal and must reach cp_session. */
    if (readback->on_air &&
        (!nonempty(readback->ssid) || readback->channel == 0u))
        return CP_SESSION_ERR_CONFIG;
    if (!broker->session_inited) {
        broker->cfg.ap_readback = *readback;
        broker->cfg.have_ap_readback = 1;
        broker->last_now_ms = now_ms;
        return CP_SESSION_OK;
    }
    broker->last_now_ms = now_ms;
    cp_session_error_t error =
        cp_session_set_ap_readback(&broker->session, readback, now_ms);
    if (error != CP_SESSION_OK) return error;
    broker->cfg.ap_readback = *readback;
    broker->cfg.have_ap_readback = 1;
    return CP_SESSION_OK;
}

cp_session_error_t cp_runtime_broker_set_receiver_active(
    cp_runtime_broker *broker, uint64_t now_ms, uint32_t epoch,
    uint32_t generation, const char *peer, bool active)
{
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (!broker->session_inited || !broker->started ||
        !broker->spp_connected || broker->spp == NULL ||
        cp_spp_dbus_state_get(broker->spp) != CP_SPP_DBUS_STATE_CONNECTED ||
        broker->terminal_gate || broker->session_failed || broker->fatal ||
        broker->state == CP_RUNTIME_BROKER_BLOCKED ||
        broker->state == CP_RUNTIME_BROKER_FATAL ||
        broker->draining)
        return CP_SESSION_ERR_STATE;
    /* Epoch proof comes before clock/expiry/policy side effects: a stale
     * callback must not terminate a new pending association. */
    if (epoch != broker->epoch) return CP_SESSION_ERR_GENERATION_MISMATCH;
    if (now_ms < broker->last_now_ms)
        return CP_SESSION_ERR_CLOCK_ROLLBACK;
    if (init_deadline_expired(broker, now_ms))
        return CP_SESSION_ERR_TIMEOUT;
    broker->last_now_ms = now_ms;
    return cp_session_set_receiver_active(&broker->session, now_ms,
                                          generation, peer, active);
}

int cp_runtime_broker_feed_hook(cp_runtime_broker *broker, uint32_t epoch,
                                const char *peer, uint8_t uuid_kind,
                                const uint8_t *data, size_t length,
                                uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized)
        return -1;
    if (!broker->started || broker->spp == NULL ||
        !broker->session_inited || !broker->spp_connected ||
        broker->draining || broker->restart_required ||
        broker->terminal_gate || broker->session_failed || broker->fatal ||
        broker->state == CP_RUNTIME_BROKER_BLOCKED ||
        broker->state == CP_RUNTIME_BROKER_FATAL)
        return 0;
    if (peer == NULL || data == NULL || length == 0) return 0;
    /* Epoch proof comes before clock/expiry/policy side effects: a stale
     * callback must not terminate a new pending association. */
    if (epoch != broker->epoch) return 0;
    if (now_ms < broker->last_now_ms) return -1;
    if (init_deadline_expired(broker, now_ms)) return -1;
    if (!policy_allows(broker, now_ms)) {
        broker->policy_blocked = true;
        broker->stats.policy_cancels++;
        cancel_internal(broker, now_ms, true, true, true, false, false,
                        CP_RUNTIME_BROKER_BLOCKED, "policy_cancel");
        return 0;
    }
    broker->last_now_ms = now_ms;
    uint32_t dropped = broker->stats.rx_dropped;
    int rc = cp_spp_dbus_deliver_rx(broker->spp, peer, uuid_kind, data,
                                    length);
    if (broker->session_failed || broker->fatal) {
        /* The callback promoted FATAL without closing or re-entering SPP.
         * We are back outside it: cancel/close once and never report the
         * failed delivery as accepted. */
        char why[sizeof(broker->error)];
        snprintf(why, sizeof(why), "%s",
                 broker->error[0] ? broker->error :
                                    "rx_session_feed_failed");
        cancel_internal(broker, now_ms, true, true, false, false, true,
                        CP_RUNTIME_BROKER_FATAL, why);
        return -1;
    }
    if (rc == CP_SPP_DBUS_OK) {
        if (broker->stats.rx_dropped == dropped)
            broker->stats.rx_packets++;
        return broker->stats.rx_dropped == dropped ? 1 : 0;
    }
    if (rc == CP_SPP_DBUS_ERR_REJECTED ||
        rc == CP_SPP_DBUS_ERR_NOT_CONNECTED) {
        broker->stats.rx_rejected++;
        return 0;
    }
    return -1;
}

cp_session_error_t cp_runtime_broker_cancel(cp_runtime_broker *broker,
                                            uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    if (now_ms < broker->last_now_ms) now_ms = broker->last_now_ms;
    cancel_internal(broker, now_ms, true, true, false, false, false,
                    CP_RUNTIME_BROKER_BLOCKED, "cancelled");
    return CP_SESSION_OK;
}

cp_runtime_broker_state_t cp_runtime_broker_state(
    const cp_runtime_broker *broker)
{
    return broker != NULL ? broker->state : CP_RUNTIME_BROKER_FATAL;
}

uint32_t cp_runtime_broker_epoch(const cp_runtime_broker *broker)
{
    return broker != NULL ? broker->epoch : 0;
}

const char *cp_runtime_broker_peer(const cp_runtime_broker *broker)
{
    return broker != NULL ? broker->bound_peer : "";
}

const char *cp_runtime_broker_error(const cp_runtime_broker *broker)
{
    return broker != NULL ? broker->error : "";
}

const cp_runtime_broker_stats_t *cp_runtime_broker_stats(
    const cp_runtime_broker *broker)
{
    return broker != NULL ? &broker->stats : NULL;
}

void cp_runtime_broker_destroy(cp_runtime_broker *broker, uint64_t now_ms)
{
    if (broker == NULL || !broker->initialized) return;
    if (now_ms < broker->last_now_ms) now_ms = broker->last_now_ms;
    cancel_internal(broker, now_ms, true, false, false, false, false,
                    CP_RUNTIME_BROKER_IDLE, "destroyed");
    memset(broker, 0, sizeof(*broker));
}
