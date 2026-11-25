/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_spp_dbus — bounded, asynchronous CommonAPI/DBus transport proposal.
 *
 * EXPERIMENTAL AND NOT CAR-VALIDATED.  This module is inert unless the caller
 * passes experimental=1 and an explicit, already-running private/session bus
 * address.  Opening does not start a bus, does not activate an OEM service, and
 * does not install or modify anything.  No callback logs payload bytes.
 *
 * The caller owns the event loop: cp_spp_dbus_tick() performs only
 * zero-timeout libdbus I/O/dispatch.  There are no blocking method calls in
 * this module.  All pending operations are fixed-bound and carry deadlines.
 * Single-threaded use: callbacks must not re-enter or close this engine.
 *
 * State:
 *   OPEN -> VALIDATING -> LISTENING -> CONNECTED
 *                 \___________ FATAL ___________/
 *
 * CP_SPP_DBUS_MAX_TX_BYTES is 1000 and at most one SendData call is in flight.
 * A SendData reply of int32 0 is only the stub's ACK; it is not delivery
 * evidence.  Any error, timeout, malformed reply, owner loss, or unexpected
 * validated-object transition is fatal; the driver never silently rebinds.
 */
#ifndef CP_SPP_DBUS_H
#define CP_SPP_DBUS_H

#include <poll.h>
#include <stddef.h>
#include <stdint.h>

#define CP_SPP_DBUS_ADAPTER_SERVICE "org.bluez.Adapter.Adapter.v2_0_lge1"
#define CP_SPP_DBUS_ADAPTER_PATH "/lge1"
#define CP_SPP_DBUS_ADAPTER_INTERFACE "org.bluez.Adapter.Adapter.v2_0"
#define CP_SPP_DBUS_SERIAL_SERVICE_PREFIX \
    "org.bluez.SerialPort.SerialPort.v2_0_"
#define CP_SPP_DBUS_SERIAL_INTERFACE "org.bluez.SerialPort.SerialPort.v2_0"
#define CP_SPP_DBUS_SERIAL_METHOD_SEND_DATA "SendData"
#define CP_SPP_DBUS_SERIAL_METHOD_DISCONNECT "Disconnect"
#define CP_SPP_DBUS_SERIAL_SIGNAL_STATUS "onStatusAttributeChanged"

#define CP_SPP_DBUS_UUID_CAFF "00000000-DECA-FADE-DECA-DEAFDECACAFF"
#define CP_SPP_DBUS_UUID_CAFE "00000000-DECA-FADE-DECA-DEAFDECACAFE"
#define CP_SPP_DBUS_UUID_CARPLAY "EC884348-CD41-40A2-9727-575D50BF1FD3"

#define CP_SPP_DBUS_MAX_TX_BYTES 1000u
#define CP_SPP_DBUS_MAX_RX_BYTES 16384u
#define CP_SPP_DBUS_MAX_XML_BYTES 65536u
#define CP_SPP_DBUS_MAX_PENDING 8u
#define CP_SPP_DBUS_MAX_PEERS 4u
#define CP_SPP_DBUS_MAX_TX_QUEUE 4u
#define CP_SPP_DBUS_MAX_NAME 256u
#define CP_SPP_DBUS_MAX_VALUE 128u
#define CP_SPP_DBUS_MAX_UUID 40u
#define CP_SPP_DBUS_STORAGE_BYTES 24576u

/* Hook UUID-kind values, repeated here so callers do not include private
 * hook headers.  Only kind 1 (CAFF listener/accessory) is accepted inbound. */
#define CP_SPP_DBUS_RX_UUID_KIND_CAFF 1u
#define CP_SPP_DBUS_RX_UUID_KIND_CAFE 2u
#define CP_SPP_DBUS_RX_UUID_KIND_CARPLAY 3u

typedef enum {
    CP_SPP_DBUS_STATE_OPEN = 0,
    CP_SPP_DBUS_STATE_VALIDATING = 1,
    CP_SPP_DBUS_STATE_LISTENING = 2,
    CP_SPP_DBUS_STATE_CONNECTED = 3,
    CP_SPP_DBUS_STATE_FATAL = 4
} cp_spp_dbus_state;

typedef enum {
    CP_SPP_DBUS_OK = 0,
    CP_SPP_DBUS_ERR_INVALID = -1,
    CP_SPP_DBUS_ERR_STATE = -2,
    CP_SPP_DBUS_ERR_BACKPRESSURE = -3,
    CP_SPP_DBUS_ERR_NOT_CONNECTED = -4,
    CP_SPP_DBUS_ERR_REJECTED = -5
} cp_spp_dbus_result;

typedef struct {
    /* Required, non-empty, explicit.  Never start/ask for a new bus. */
    const char *bus_address;
    /* Optional overrides; defaults are the static protocol facts above. */
    const char *adapter_service;
    const char *adapter_path;
    const char *adapter_interface;
    const char *serial_service_prefix;
    const char *serial_interface;
    /* Optional; defaults CAFF/CAFE.  Must be distinct. */
    const char *uuid_accessory;
    const char *uuid_phone;
    /* REQUIRED: OEM SDP listener name.  Vendor ListenSPP signature is
     * (UUID, name), so this is the second argument, not the first. */
    const char *service_name;
    /* Production defaults to uid 0 (root).  Tests may set an explicit uid. */
    int expected_owner_uid_set;
    uint32_t expected_owner_uid;
    uint32_t control_timeout_ms; /* default 3000 */
    uint32_t send_timeout_ms;    /* default 1000 */
    size_t max_tx_bytes;         /* default and hard limit 1000 */
    int experimental;            /* MUST be 1 */
} cp_spp_dbus_config;

typedef struct {
    void (*on_state)(void *user, cp_spp_dbus_state old_state,
                     cp_spp_dbus_state new_state);
    void (*on_validated)(void *user);
    void (*on_status)(void *user, const char *object_path,
                      const char *status);
    void (*on_removed)(void *user, const char *object_path);
    void (*on_fatal)(void *user, const char *reason);
    /* Borrowed peer/data for the duration of the callback only. */
    void (*on_rx)(void *user, const char *peer, uint8_t uuid_kind,
                  const uint8_t *data, size_t length);
    /* Stub ACK only; never delivery.  Optional. */
    void (*on_stub_ack)(void *user, size_t length);
} cp_spp_dbus_callbacks;

typedef struct cp_spp_dbus cp_spp_dbus;

typedef union {
    max_align_t align;
    unsigned char bytes[CP_SPP_DBUS_STORAGE_BYTES];
} cp_spp_dbus_storage;

/* Open only a private connection to the supplied existing address.  The
 * address must not be "autolaunch:"; no bus is created.  No Hello, method
 * call, match rule, or listener is started here. */
int cp_spp_dbus_open(cp_spp_dbus **engine, cp_spp_dbus_storage *storage,
                     const cp_spp_dbus_config *config,
                     const cp_spp_dbus_callbacks *callbacks, void *user,
                     char *error, size_t error_size);

/* Asynchronous Hello, AddMatch, ListNames, owner/uid, and Adapter XML
 * validation.  Caller must tick until on_validated/fatal. */
int cp_spp_dbus_start(cp_spp_dbus *engine, uint64_t now_ms);

/* Zero-timeout read/write/dispatch and deadline processing. */
int cp_spp_dbus_tick(cp_spp_dbus *engine, uint64_t now_ms);

/* Send ListenSPP only after on_validated.  Error 3 is fatal and never
 * assumes ownership of a foreign listener.  An already-registered SerialPort
 * object is enumerated during validation; the first connection is completed
 * after our own ListenSPP succeeds, even when no status signal arrives. */
int cp_spp_dbus_listen(cp_spp_dbus *engine, uint64_t now_ms);

/* Send StopListenSPP only if this driver successfully created the listener.
 * Idempotent: stopping an unowned/already-stopped listener returns OK without
 * sending anything.  Never stops a listener this process did not start. */
int cp_spp_dbus_stop(cp_spp_dbus *engine, uint64_t now_ms);

/* Queue <=1000 bytes for one-in-flight SendData.  CP_SPP_DBUS_ERR_BACKPRESSURE
 * means the bounded queue is full; it is not a delivery success. */
int cp_spp_dbus_send(cp_spp_dbus *engine, const uint8_t *data, size_t length,
                     uint64_t now_ms);

/* Bind a hook datagram to the validated D-Bus peer.  Only CAFF is accepted.
 * Returns 0 and invokes on_rx if accepted, otherwise a negative error. */
int cp_spp_dbus_deliver_rx(cp_spp_dbus *engine, const char *peer,
                           uint8_t uuid_kind, const uint8_t *data,
                           size_t length);

int cp_spp_dbus_pollfd(const cp_spp_dbus *engine, struct pollfd *pfd);
int cp_spp_dbus_timeout_ms(const cp_spp_dbus *engine, uint64_t now_ms);
int cp_spp_dbus_next_deadline(const cp_spp_dbus *engine,
                              uint64_t *deadline_ms);

cp_spp_dbus_state cp_spp_dbus_state_get(const cp_spp_dbus *engine);
int cp_spp_dbus_adapter_validated(const cp_spp_dbus *engine);
int cp_spp_dbus_listener_owned(const cp_spp_dbus *engine);
/* Monotonic per-driver connection counter.  It advances only when a fresh
 * connected object is confirmed (status signal or enumerated at attach), so a
 * reconnect by the same peer is distinguishable from stale per-connection
 * state.  0 means no connection has been confirmed yet. */
uint32_t cp_spp_dbus_connect_generation(const cp_spp_dbus *engine);
/* True when the current connection was completed from an already-registered
 * SerialPort object at attach (no status signal). */
int cp_spp_dbus_attached(const cp_spp_dbus *engine);
size_t cp_spp_dbus_tx_queued(const cp_spp_dbus *engine);
const char *cp_spp_dbus_error(const cp_spp_dbus *engine);
/* Copy the peer bound by the validated CommonAPI service/object identity.
 * No hook datagram is allowed to choose this identity. Requires cap >= 18. */
int cp_spp_dbus_peer(const cp_spp_dbus *engine, char *peer, size_t cap);

/* Best-effort StopListenSPP only if owned, then close the private connection.
 * This never blocks and never tears down a listener it did not create. */
void cp_spp_dbus_close(cp_spp_dbus *engine);

#endif /* CP_SPP_DBUS_H */
