/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Asynchronous, nonblocking client for the root MFi helper RPC.
 *
 * This module is intentionally NOT a cp_boot_auth_provider.  A successful
 * completion means only "bounded certificate bytes" or "bounded signature
 * bytes" arrived from the helper.  It never means phone authentication
 * succeeded, and it never fabricates a certificate. cp_auth_broker connects
 * this client to the opt-in deferred cp_bootstrap/cp_session interface without
 * blocking the serialized link loop. Product activation remains separate.
 *
 * Usage:
 *   - init once with experimental=true (production uses the fixed helper
 *     socket; tests compile path/uid overrides only);
 *   - begin_certificate()/begin_signature() with the caller's current
 *     generation and an absolute monotonic deadline;
 *   - call service() from the caller's poll loop.  The module never calls
 *     poll(), sleeps or blocks: it performs nonblocking connect/send/recvmsg
 *     progress only.  The caller owns poll(fd, poll_events), the deadline and
 *     the scheduling that keeps iAP ACKs moving;
 *   - when completion_ready is set, take_completion() exactly once;
 *   - cancel(generation) aborts a matching in-flight operation.
 *
 * Bounds are deliberately lower than the blocking adapter:
 * certificates > 2048 bytes and signatures > 1024 bytes fail before any
 * future bootstrap use.  CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE fits
 * CP_WIRE_MAX_AUTH_BLOB.
 *
 * Fixed storage only: no allocation, threads, stdio, logging or raw helper
 * failure payloads.  Single-threaded use; one operation in flight.
 */

#ifndef CP_OEM_AUTH_ASYNC_H
#define CP_OEM_AUTH_ASYNC_H

#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Read-only reuse of the published wire constants and header-only BE codecs.
 * Do not link cp_oem_auth.c: this module has no pthread/blocking dependency. */
#include "cp_oem_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Conservative completion cap for the current bootstrap buffer. */
#define CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE 2048u
#define CP_OEM_AUTH_ASYNC_MAX_SIGNATURE CP_OEM_AUTH_MAX_SIGNATURE
/* Read enough to classify a helper certificate up to its 4096-byte hard cap,
 * then reject it as too large for the bootstrap before handing it on. */
#define CP_OEM_AUTH_ASYNC_RX_SIZE \
    (CP_OEM_AUTH_RPC_RESPONSE_LEN + CP_OEM_AUTH_MAX_CERTIFICATE)

typedef enum {
    CP_OEM_AUTH_ASYNC_STATE_IDLE = 0,
    CP_OEM_AUTH_ASYNC_STATE_CONNECTING,
    CP_OEM_AUTH_ASYNC_STATE_SENDING,
    CP_OEM_AUTH_ASYNC_STATE_RECEIVING,
    /* Completion ready; take_completion() must be called before begin again. */
    CP_OEM_AUTH_ASYNC_STATE_DONE
} cp_oem_auth_async_state;

typedef enum {
    CP_OEM_AUTH_ASYNC_OP_NONE = 0,
    CP_OEM_AUTH_ASYNC_OP_CERTIFICATE,
    CP_OEM_AUTH_ASYNC_OP_SIGNATURE
} cp_oem_auth_async_op;

typedef enum {
    CP_OEM_AUTH_ASYNC_OK = 0,
    CP_OEM_AUTH_ASYNC_ERR_ARGUMENT,
    CP_OEM_AUTH_ASYNC_ERR_DISABLED,
    CP_OEM_AUTH_ASYNC_ERR_BUSY,
    CP_OEM_AUTH_ASYNC_ERR_STATE,
    CP_OEM_AUTH_ASYNC_ERR_GENERATION,
    CP_OEM_AUTH_ASYNC_ERR_CLOCK,
    CP_OEM_AUTH_ASYNC_ERR_DEADLINE,
    CP_OEM_AUTH_ASYNC_ERR_PATH,
    CP_OEM_AUTH_ASYNC_ERR_CONNECT,
    CP_OEM_AUTH_ASYNC_ERR_PEER,
    CP_OEM_AUTH_ASYNC_ERR_SEND,
    CP_OEM_AUTH_ASYNC_ERR_RECV,
    CP_OEM_AUTH_ASYNC_ERR_PROTOCOL,
    CP_OEM_AUTH_ASYNC_ERR_TRUNCATED,
    CP_OEM_AUTH_ASYNC_ERR_RIGHTS,
    CP_OEM_AUTH_ASYNC_ERR_TOO_LARGE,
    CP_OEM_AUTH_ASYNC_ERR_CHALLENGE_LENGTH,
    CP_OEM_AUTH_ASYNC_ERR_HELPER,
    CP_OEM_AUTH_ASYNC_ERR_CANCELLED,
    CP_OEM_AUTH_ASYNC_ERR_TIMEOUT,
    CP_OEM_AUTH_ASYNC_ERR_FD,
    CP_OEM_AUTH_ASYNC_ERR_INTERNAL
} cp_oem_auth_async_error;

/* Sanitised completion. On failure bytes[] is zero and length is zero; only
 * enums, generation, op and request_id survive. No raw helper payload is
 * exposed on failure. helper_status is meaningful only for ERR_HELPER. */
typedef struct {
    cp_oem_auth_async_op op;
    uint64_t generation;
    uint32_t request_id;
    cp_oem_auth_async_error error;
    cp_oem_auth_error helper_status;
    uint32_t length;
    uint8_t bytes[CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE];
} cp_oem_auth_async_completion;

typedef struct {
    bool experimental; /* required production opt-in */
#ifdef CP_OEM_AUTH_ASYNC_TEST
    const char *socket_path; /* tests only; NULL selects the default */
    uint32_t expected_peer_uid; /* tests only; production is always root */
#endif
} cp_oem_auth_async_config;

/* Caller-owned state. fd/poll_events/deadline_ms/state are intentionally
 * public for the caller's poll loop; treat the rest as private. */
typedef struct {
    bool initialized;
    bool experimental;
    char socket_path[CP_OEM_AUTH_SOCKET_PATH_MAX];
    int fd;
    short poll_events;
    uint64_t deadline_ms;
    uint64_t last_now_ms;
    cp_oem_auth_async_state state;
    cp_oem_auth_async_op op;
    uint64_t generation;
    uint32_t request_id;
    uint32_t next_request_id;
    bool completion_ready;
    cp_oem_auth_async_completion completion;

    uint8_t tx[CP_OEM_AUTH_RPC_REQUEST_LEN +
               CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD];
    uint32_t tx_len;
    uint8_t rx[CP_OEM_AUTH_ASYNC_RX_SIZE];

    uint32_t rights_fds_received;
    uint32_t rights_fds_closed;

#ifdef CP_OEM_AUTH_ASYNC_TEST
    uint32_t expected_peer_uid;
#endif
} cp_oem_auth_async;

cp_oem_auth_async_error cp_oem_auth_async_init(
    cp_oem_auth_async *async, const cp_oem_auth_async_config *config);

/* Arm one operation. The request is not connected until service() runs.
 * deadline_ms is absolute monotonic and must be greater than now_ms. */
cp_oem_auth_async_error cp_oem_auth_async_begin_certificate(
    cp_oem_auth_async *async, uint64_t generation,
    uint64_t now_ms, uint64_t deadline_ms);
cp_oem_auth_async_error cp_oem_auth_async_begin_signature(
    cp_oem_auth_async *async, uint64_t generation,
    const uint8_t *digest, size_t digest_len,
    uint64_t now_ms, uint64_t deadline_ms);

/* Progress nonblocking connect/send/recvmsg. revents is the caller's poll
 * result for async->fd (0 is allowed; no poll is performed here). Returns OK
 * while work remains; returns a terminal error when a failure completion was
 * produced. Check completion_ready/take_completion after every call. */
cp_oem_auth_async_error cp_oem_auth_async_service(
    cp_oem_auth_async *async, short revents, uint64_t now_ms);

/* Copy and clear a completion exactly once. Returns false if none is ready. */
bool cp_oem_auth_async_take_completion(
    cp_oem_auth_async *async, cp_oem_auth_async_completion *completion);

/* Cancel only the operation with the matching generation. A stale generation
 * is refused and leaves the newer operation untouched. A cancelled operation
 * yields one ERR_CANCELLED completion. */
cp_oem_auth_async_error cp_oem_auth_async_cancel(
    cp_oem_auth_async *async, uint64_t generation);

/* Close and wipe all state. Does not produce a completion. */
void cp_oem_auth_async_destroy(cp_oem_auth_async *async);

cp_oem_auth_async_state cp_oem_auth_async_get_state(
    const cp_oem_auth_async *async);
int cp_oem_auth_async_get_fd(const cp_oem_auth_async *async);
short cp_oem_auth_async_get_poll_events(const cp_oem_auth_async *async);
bool cp_oem_auth_async_completion_ready(const cp_oem_auth_async *async);
const char *cp_oem_auth_async_error_name(cp_oem_auth_async_error error);
const char *cp_oem_auth_async_state_name(cp_oem_auth_async_state state);

#ifdef __cplusplus
}
#endif

#endif /* CP_OEM_AUTH_ASYNC_H */
