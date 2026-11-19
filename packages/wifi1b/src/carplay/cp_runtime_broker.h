/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_runtime_broker — caller-owned, unactivated serialized CarPlay runtime.
 *
 * OFFLINE IMPLEMENTATION ONLY.  It joins an already-validated private-bus SPP
 * transport (cp_spp_dbus), a credential-checked hook receiver (cp_spp_socket),
 * the generation-bound cp_session wrapper and the reviewed asynchronous
 * cp_auth_broker.  It does not start a product daemon, touch the installer,
 * activate a service, start a global receiver, synthesize an identity, or
 * claim MFi/projection success.
 *
 * The caller owns the event loop and supplies:
 *   - an explicit private bus address and OEM service_name (experimental=1);
 *   - the complete OEM/session identity and AP readback (copied by cp_session);
 *   - consent and USB-active policy callbacks;
 *   - an explicit initiator role and finite init/drain deadlines;
 *   - an optional caller-owned cp_spp_socket that has already been opened by
 *     the credential-checked RX path.  Without it, no hook RX is dispatched.
 *
 * The broker never blocks in cp_runtime_broker_tick(): it performs zero-timeout
 * SPP dispatch, nonblocking helper/socket progress, bounded RX/TX work and
 * deadline checks.  cp_runtime_broker_start()/restart() open a private bus and
 * may block inside libdbus setup; that setup cost is not claimed bounded.
 *
 * Lifecycle:
 *   empty -> start -> wait SPP -> drain old hook RX -> bind validated peer ->
 *   session init/auth -> active.
 * The finite init deadline is armed by start()/restart() before the private
 * bus is opened and covers waiting for SPP, draining stale hook RX, session
 * init and auth.  It is never refreshed by a late session start or a late
 * auth success.  Any policy/consent/USB cancel, SPP fatal/owner loss, auth or
 * session failure closes the SPP connection (discarding its local queues),
 * cancels the session/helper and raises a restart gate.  An ordinary SPP
 * disconnect keeps the owned listener: the session is cancelled and the broker
 * waits for a fresh connected confirmation, whose monotonically increasing
 * connection generation discards all old per-connection state (even when the
 * same phone reconnects).  Pre-session hook frames are preserved in a bounded
 * buffer and replayed after the session starts: on initial attach, and for
 * frames that arrive after a restart for the new epoch.  Frames from the
 * replaced connection are cleared on disconnect, reconnect or restart
 * because the hook wire format carries no connection generation.  No new
 * epoch starts until the
 * caller explicitly restarts and the old hook RX queue is drained to EAGAIN or
 * the bounded drain deadline fails closed.  A new peer or a reconnect always
 * requires that drain; peer bytes have no auth generation.  The runtime epoch
 * is the caller's trust scope: receiver observations and external hook feeds
 * must present the current epoch.  Epoch exhaustion is fatal and never wraps.
 *
 * TX consumes cp_session output only after cp_spp_dbus_send() accepts the
 * local queue.  A stub ACK is never treated as remote delivery or projection.
 */
#ifndef CP_RUNTIME_BROKER_H
#define CP_RUNTIME_BROKER_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp_auth_broker.h"
#include "cp_session.h"
#include "cp_spp_dbus.h"
#include "cp_spp_socket.h"

#define CP_RUNTIME_BROKER_MAX_TX_CHUNKS 4u
#define CP_RUNTIME_BROKER_MAX_RX_PER_TICK 32u
#define CP_RUNTIME_BROKER_MAX_INIT_MS 120000u
#define CP_RUNTIME_BROKER_MAX_DRAIN_MS 10000u
#define CP_RUNTIME_BROKER_MAX_POLLFDS 3u
/* Bounded pre-session RX preserved for the initial attach and for frames
 * arriving after a restart.  The buffer is cleared on disconnect, reconnect
 * and restart so bytes from the replaced connection are never replayed; the
 * hook wire format carries no connection generation. */
#define CP_RUNTIME_BROKER_MAX_EARLY_RX 4u

typedef enum {
    CP_RUNTIME_BROKER_IDLE = 0,
    CP_RUNTIME_BROKER_WAIT_SPP = 1,
    CP_RUNTIME_BROKER_DRAINING = 2,
    CP_RUNTIME_BROKER_STARTING = 3,
    CP_RUNTIME_BROKER_ACTIVE = 4,
    CP_RUNTIME_BROKER_BLOCKED = 5,
    CP_RUNTIME_BROKER_FATAL = 6
} cp_runtime_broker_state_t;

typedef struct {
    uint32_t epochs;
    uint32_t sessions;
    uint32_t spp_connects;
    uint32_t spp_disconnects;
    uint32_t spp_reconnects;
    uint32_t early_rx;
    uint32_t drains;
    uint32_t drain_timeouts;
    uint32_t rx_packets;
    uint32_t rx_rejected;
    uint32_t rx_dropped;
    uint32_t tx_chunks;
    uint32_t tx_bytes;
    uint32_t tx_backpressure;
    uint32_t policy_cancels;
    uint32_t auth_failures;
    uint32_t session_failures;
    uint32_t init_timeouts;
    uint32_t helper_completions;
    uint32_t stub_acks;
} cp_runtime_broker_stats_t;

typedef struct {
    /* All const char * and array pointers below are caller-owned and must
     * remain valid until cp_runtime_broker_destroy().  The broker copies no
     * unbounded text and never takes ownership of them. */
    /* Both must be 1.  No product daemon or installer activates this. */
    int experimental;
    int enabled;
    /* Explicit private bus address, service_name, owner UID and timeouts. */
    cp_spp_dbus_config spp;
    /* Explicit private helper; experimental=1 and no global activation. */
    cp_oem_auth_async_config auth;
    /* Complete caller-supplied OEM identity, AP and link configuration.
     * cp_session copies the values on each new epoch. */
    cp_session_config_t session;
    cp_link_role_t role;
    /* Required trusted policy sources.  Missing callbacks fail init. */
    cp_boot_consent_fn consent;
    cp_boot_usb_active_fn usb_active;
    void *policy_context;
    /* Required caller-attested AP readback; never derived from a network bool.
     * An explicit off-air readback is valid and starts the session gated until
     * a fresh on-air readback arrives.  When on_air=1, ssid must be nonempty
     * and channel nonzero. */
    cp_boot_ap_readback ap_readback;
    int have_ap_readback;
    /* Required finite setup deadlines.  init_timeout_ms is capped at
     * CP_RUNTIME_BROKER_MAX_INIT_MS and starts at start()/restart() before
     * bus setup; it covers SPP wait, drain and auth.  drain_timeout_ms is
     * capped at CP_RUNTIME_BROKER_MAX_DRAIN_MS and is also capped by the
     * remaining total init budget.  0 is rejected. */
    uint32_t init_timeout_ms;
    uint32_t drain_timeout_ms;
    /* Optional per-tick bounds.  0 selects 2 TX chunks and 8 RX datagrams. */
    uint32_t max_tx_chunks_per_tick;
    uint32_t max_rx_per_tick;
    /* Optional caller-owned, already-open credential-checked hook receiver. */
    cp_spp_socket *rx;
} cp_runtime_broker_config_t;

/* Caller-owned fixed storage.  This embeds cp_session (roughly 500 KiB at
 * default limits) plus the 24 KiB SPP storage; allocate it statically or on
 * the heap, never on a small task stack. */
typedef struct {
    char peer[18];
    uint8_t uuid_kind;
    uint16_t length;
    uint8_t bytes[CP_SPP_SOCKET_MAX_PAYLOAD];
} cp_runtime_broker_early_rx_t;

typedef struct {
    bool initialized;
    bool started;
    bool fatal;
    bool restart_required;
    bool terminal_gate;
    bool auth_failed;
    bool session_failed;
    bool init_timed_out;
    bool policy_blocked;
    bool draining;
    bool drain_quiescent;
    bool spp_connected;
    bool session_inited;
    bool auth_inited;
    uint32_t epoch;
    cp_runtime_broker_state_t state;
    cp_runtime_broker_config_t cfg;
    cp_spp_dbus_storage spp_storage;
    cp_spp_dbus *spp;
    cp_spp_socket *rx;
    cp_session_t session;
    cp_auth_broker auth;
    uint64_t last_now_ms;
    uint64_t init_deadline_ms;
    uint64_t drain_deadline_ms;
    uint32_t spp_connect_generation;
    bool preserve_early_rx;
    uint8_t early_count;
    uint8_t early_replayed;
    cp_runtime_broker_early_rx_t early[CP_RUNTIME_BROKER_MAX_EARLY_RX];
    char peer[18];
    char bound_peer[18];
    char error[192];
    cp_runtime_broker_stats_t stats;
} cp_runtime_broker;

/* Validate/copy configuration only.  Does not open a bus, socket or helper.
 * The broker storage must be zero-initialized or previously destroyed; never
 * re-initialize an active broker (call destroy first). */
cp_session_error_t cp_runtime_broker_init(
    cp_runtime_broker *broker, const cp_runtime_broker_config_t *config);

/* Open the explicit private SPP bus and begin waiting for validation.  This
 * setup may block inside libdbus; no bounded-wall-clock claim is made, but the
 * finite init deadline is armed before setup and includes this wait. */
cp_session_error_t cp_runtime_broker_start(cp_runtime_broker *broker,
                                           uint64_t now_ms);

/* Explicit new SPP epoch after a restart gate or deliberate re-association.
 * Requires policy to be safe, closes any old SPP connection (discarding its
 * local queues), arms a fresh finite init deadline before bus setup, then
 * waits for a new validated peer and drains old hook RX before a session
 * starts.  The drain budget cannot extend the total init deadline. */
cp_session_error_t cp_runtime_broker_restart(cp_runtime_broker *broker,
                                             uint64_t now_ms);

/* Fill up to CP_RUNTIME_BROKER_MAX_POLLFDS caller pollfds: SPP bus, hook RX,
 * helper. */
size_t cp_runtime_broker_pollfds(cp_runtime_broker *broker,
                                 struct pollfd *pfds, size_t capacity);

/* Bounded zero-timeout work.  pfds/nfds are the caller's poll result; only
 * fd-matching revents are used and nfds must be 0..CP_RUNTIME_BROKER_MAX_POLLFDS
 * (larger values are rejected).  The caller's now_ms must be monotonic.
 * A tick that reads hook RX uses one CP_SPP_SOCKET_MAX_FRAME stack buffer
 * (about 64 KiB); run it on a thread with adequate stack, or pass rx=NULL. */
cp_session_error_t cp_runtime_broker_tick(cp_runtime_broker *broker,
                                          const struct pollfd *pfds,
                                          size_t nfds, uint64_t now_ms);

/* Earliest absolute deadline, or UINT64_MAX when no timer is armed.  While
 * waiting for SPP this includes the total init deadline. */
uint64_t cp_runtime_broker_next_deadline(const cp_runtime_broker *broker);

/* Forward a caller-trusted AP readback; never inferred from network state.
 * on_air=0 is an explicit withdrawal and is forwarded immediately: it gates
 * new offers and clears an active receiver through cp_session.  on_air=1
 * requires a nonempty ssid and nonzero channel before any state is touched.
 * The clock is validated before cfg is changed.  For an initialized session,
 * cfg is updated only after cp_session accepts the readback.  The last
 * accepted AP readback is retained across restart unless explicitly
 * withdrawn, so a stored withdrawal cannot resurrect the old on-air state.
 * Receiver attestations are separate and must be refreshed per runtime epoch.
 */
cp_session_error_t cp_runtime_broker_set_ap_readback(
    cp_runtime_broker *broker, const cp_boot_ap_readback *readback,
    uint64_t now_ms);

/* Forward the caller's trusted receiver attestation.  This broker never
 * derives receiver activity from SPP status, RX bytes or a boolean.
 * epoch must equal cp_runtime_broker_epoch(); the session generation alone is
 * not sufficient because cp_session generation values can repeat across
 * restarts.  cp_session additionally verifies generation/peer/AP/auth/policy
 * consistency.  The caller must refresh its attestation for each epoch. */
cp_session_error_t cp_runtime_broker_set_receiver_active(
    cp_runtime_broker *broker, uint64_t now_ms, uint32_t epoch,
    uint32_t generation, const char *peer, bool active);

/* Feed one already-credential-checked hook datagram from a caller bridge.
 * epoch must equal cp_runtime_broker_epoch(); stale-epoch data is dropped.
 * Trusted policy is rechecked before any clock/feed side effect.  Returns 1
 * accepted, 0 dropped because the broker is inactive/draining/stale-epoch,
 * negative on error.  The real socket drain remains the EAGAIN gate; the SPP
 * driver still validates the CAFF kind and the bound peer. */
int cp_runtime_broker_feed_hook(cp_runtime_broker *broker, uint32_t epoch,
                                const char *peer, uint8_t uuid_kind,
                                const uint8_t *data, size_t length,
                                uint64_t now_ms);

/* Caller cancellation: close SPP (clearing local queues), cancel session and
 * helper, and require an explicit restart/new SPP epoch. */
cp_session_error_t cp_runtime_broker_cancel(cp_runtime_broker *broker,
                                            uint64_t now_ms);

cp_runtime_broker_state_t cp_runtime_broker_state(
    const cp_runtime_broker *broker);
uint32_t cp_runtime_broker_epoch(const cp_runtime_broker *broker);
const char *cp_runtime_broker_peer(const cp_runtime_broker *broker);
const char *cp_runtime_broker_error(const cp_runtime_broker *broker);
const cp_runtime_broker_stats_t *cp_runtime_broker_stats(
    const cp_runtime_broker *broker);

/* Close SPP, cancel session/helper and wipe broker state.  Does not close the
 * caller-owned rx socket. */
void cp_runtime_broker_destroy(cp_runtime_broker *broker, uint64_t now_ms);

#endif /* CP_RUNTIME_BROKER_H */
