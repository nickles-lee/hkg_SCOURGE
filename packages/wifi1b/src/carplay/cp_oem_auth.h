/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Hardware-backed MFi provider adapter for the wireless CarPlay bootstrap.
 *
 * Production path: a tiny root helper (cp_native_auth) validates the pinned
 * libCoreUtils.so by SHA-256, resolves the three MFiPlatform entry points and
 * serves certificate/signature requests over a root-only Unix SOCK_SEQPACKET
 * socket.  The in-process adapter talks only to that helper; it never dlopens
 * libCoreUtils itself and never touches /dev/i2c-1.
 *
 * The helper is opt-in: without --serve --experimental it exits without
 * loading the library or creating a socket.  The in-process factory likewise
 * refuses to activate unless the caller sets experimental=true.
 *
 * BUILD-ONLY / UNACTIVATED / NOT CAR-VALIDATED: compiling this adapter and
 * helper proves only that the pinned artifact wires up on the build host.  It
 * does not prove target loader dependency resolution, rootfs aliases, OEM
 * helper execution, chip access or vehicle behavior.  Successful certificate
 * or signature bytes are NOT authentication success; only the phone's AA05
 * may set that in cp_bootstrap.
 *
 * Provider context lifetime: cp_boot_auth_provider.context points at the
 * caller-owned cp_oem_auth.  Detach it from every cp_bootstrap/cp_session
 * (for example cp_bootstrap_set_auth_provider(session, NULL)) before calling
 * cp_oem_auth_destroy.  No callback may run after destroy.
 *
 * No certificate, signature, private key or OEM log content is ever logged.
 * The adapter returns bytes to cp_boot; it never marks phone authentication
 * successful.
 *
 * The test-only mock entry point is compiled only with CP_OEM_AUTH_TEST.  A
 * production build has no fake provider.
 *
 * Verified OEM ABI (assembly, no OEM execution):
 *   int MFiPlatform_Initialize(void);
 *   int MFiPlatform_CopyCertificate(void **out, size_t *out_len);
 *   int MFiPlatform_CreateSignature(const void *digest, size_t digest_len,
 *                                   void **out, size_t *out_len);
 * The extra decompiler arguments are logging helpers, not public ABI.
 */

#ifndef CP_OEM_AUTH_H
#define CP_OEM_AUTH_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "cp_bootstrap.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pinned own-firmware libCoreUtils.so.  The build script and helper both use
 * this value; a mismatch is a hard refusal. */
#define CP_OEM_AUTH_COREUTILS_SHA256 \
    "9104e2c16ee27e647fac090ee7f3aa4cc686b8caaea1dc24763af2950c0ebfae"
#define CP_OEM_AUTH_COREUTILS_DEFAULT_PATH "/app/lib/libCoreUtils.so"
#define CP_OEM_AUTH_MFI_INITIALIZE_SYMBOL "MFiPlatform_Initialize"
#define CP_OEM_AUTH_MFI_CERTIFICATE_SYMBOL "MFiPlatform_CopyCertificate"
#define CP_OEM_AUTH_MFI_SIGNATURE_SYMBOL "MFiPlatform_CreateSignature"
#define CP_OEM_AUTH_MFI_INITIALIZE_ADDR 0x8129fULL
#define CP_OEM_AUTH_MFI_CERTIFICATE_ADDR 0x80d6cULL
#define CP_OEM_AUTH_MFI_SIGNATURE_ADDR 0x8068aULL

#define CP_OEM_AUTH_CHALLENGE_SHA1 20u
#define CP_OEM_AUTH_CHALLENGE_SHA256 32u
#define CP_OEM_AUTH_MAX_CERTIFICATE 4096u
#define CP_OEM_AUTH_MAX_SIGNATURE 1024u
#define CP_OEM_AUTH_SOCKET_PATH_MAX 108u

/* Root-only RPC.  Fields are transmitted big-endian; use the inline helpers
 * below so a later broker does not depend on host struct layout. */
#define CP_OEM_AUTH_RPC_MAGIC 0x53434131u /* "SCA1" */
#define CP_OEM_AUTH_RPC_VERSION 1u
#define CP_OEM_AUTH_RPC_REQUEST_LEN 20u
#define CP_OEM_AUTH_RPC_RESPONSE_LEN 24u
#define CP_OEM_AUTH_RPC_MAX_FRAME 8192u
#define CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD 32u
#define CP_OEM_AUTH_RPC_DEFAULT_TIMEOUT_MS 5000u
#define CP_OEM_AUTH_RPC_MAX_TIMEOUT_MS 60000u
#define CP_OEM_AUTH_RPC_DEFAULT_DIR "/run/scourge-carplay-auth"
#define CP_OEM_AUTH_RPC_DEFAULT_SOCKET CP_OEM_AUTH_RPC_DEFAULT_DIR "/auth.sock"

#define CP_OEM_AUTH_RPC_OP_CERTIFICATE 1u
#define CP_OEM_AUTH_RPC_OP_SIGNATURE 2u

typedef enum {
    CP_OEM_AUTH_OK = 0,
    CP_OEM_AUTH_ERR_ARGUMENT = 1,
    CP_OEM_AUTH_ERR_DISABLED = 2,
    CP_OEM_AUTH_ERR_POLICY = 3,
    CP_OEM_AUTH_ERR_LIBRARY = 4,
    CP_OEM_AUTH_ERR_HASH = 5,
    CP_OEM_AUTH_ERR_SYMBOL = 6,
    CP_OEM_AUTH_ERR_INIT = 7,
    CP_OEM_AUTH_ERR_CERTIFICATE = 8,
    CP_OEM_AUTH_ERR_CHALLENGE_LENGTH = 9,
    CP_OEM_AUTH_ERR_SIGNATURE = 10,
    CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL = 11,
    CP_OEM_AUTH_ERR_OOM = 12,
    CP_OEM_AUTH_ERR_RPC = 13,
    CP_OEM_AUTH_ERR_RPC_TIMEOUT = 14,
    CP_OEM_AUTH_ERR_RPC_PEER = 15,
    CP_OEM_AUTH_ERR_RPC_PROTOCOL = 16,
    CP_OEM_AUTH_ERR_INTERNAL = 17
} cp_oem_auth_error;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t op;
    uint32_t request_id;
    uint32_t payload_len;
    uint32_t reserved;
} cp_oem_auth_rpc_request;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t op;
    uint32_t request_id;
    int32_t status;
    uint32_t payload_len;
    uint32_t reserved;
} cp_oem_auth_rpc_response;

/* Reusable native binding used by the helper and by unit-test mocks.  A
 * backend returns 0 on success, or a positive cp_oem_auth_error.  On success
 * it owns a malloc-compatible buffer that release() frees. */
typedef struct {
    void *context;
    int (*initialize)(void *context);
    int (*copy_certificate)(void *context, void **out, size_t *out_len);
    int (*create_signature)(void *context, const void *digest,
                            size_t digest_len, void **out, size_t *out_len);
    void (*release)(void *context, void *buffer, size_t len);
} cp_oem_auth_native_ops;

typedef struct {
    const char *socket_path; /* NULL selects the default */
    /* Total absolute deadline budget for one RPC attempt: connect + request
     * send + response receive.  It is one monotonic deadline, not a per-phase
     * reset.  The helper may remain stuck in OEM/chip code after the client
     * times out; this bounds the caller, not a hardware watchdog. */
    uint32_t timeout_ms;     /* 0 selects the default */
    bool experimental;       /* required explicit activation */
#ifdef CP_OEM_AUTH_TEST
    uint32_t expected_peer_uid; /* tests only; production is always root */
#endif
} cp_oem_auth_rpc_config;

typedef struct {
    pthread_mutex_t lock;
    bool initialized;
    bool mock;
    bool rpc;
    bool experimental;
    bool backend_initialized;
    cp_oem_auth_error backend_error;
    cp_oem_auth_native_ops ops;
    char socket_path[CP_OEM_AUTH_SOCKET_PATH_MAX];
    uint32_t timeout_ms;
    int fd;
    uint32_t next_request_id;
    cp_oem_auth_error last_error;
    uint8_t rx[CP_OEM_AUTH_RPC_MAX_FRAME];
    uint8_t tx[CP_OEM_AUTH_RPC_REQUEST_LEN +
              CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD];
#ifdef CP_OEM_AUTH_TEST
    uint32_t expected_peer_uid;
    bool force_destroy_lock_failure;
#endif
} cp_oem_auth;

/* ---------------------------------------------------------------- */
/* Wire helpers (header-only so the tiny helper needs no client code) */
/* ---------------------------------------------------------------- */

static inline uint16_t cp_oem_auth_get_u16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static inline uint32_t cp_oem_auth_get_u32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline void cp_oem_auth_put_u16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static inline void cp_oem_auth_put_u32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static inline void cp_oem_auth_rpc_request_encode(
    uint8_t out[CP_OEM_AUTH_RPC_REQUEST_LEN],
    const cp_oem_auth_rpc_request *request)
{
    cp_oem_auth_put_u32(out, request->magic);
    cp_oem_auth_put_u16(out + 4u, request->version);
    cp_oem_auth_put_u16(out + 6u, request->op);
    cp_oem_auth_put_u32(out + 8u, request->request_id);
    cp_oem_auth_put_u32(out + 12u, request->payload_len);
    cp_oem_auth_put_u32(out + 16u, request->reserved);
}

static inline void cp_oem_auth_rpc_response_encode(
    uint8_t out[CP_OEM_AUTH_RPC_RESPONSE_LEN],
    const cp_oem_auth_rpc_response *response)
{
    cp_oem_auth_put_u32(out, response->magic);
    cp_oem_auth_put_u16(out + 4u, response->version);
    cp_oem_auth_put_u16(out + 6u, response->op);
    cp_oem_auth_put_u32(out + 8u, response->request_id);
    cp_oem_auth_put_u32(out + 12u, (uint32_t)response->status);
    cp_oem_auth_put_u32(out + 16u, response->payload_len);
    cp_oem_auth_put_u32(out + 20u, response->reserved);
}

static inline bool cp_oem_auth_rpc_request_decode(
    const uint8_t *in, size_t len, cp_oem_auth_rpc_request *out)
{
    if (in == NULL || out == NULL || len != CP_OEM_AUTH_RPC_REQUEST_LEN) {
        return false;
    }
    out->magic = cp_oem_auth_get_u32(in);
    out->version = cp_oem_auth_get_u16(in + 4u);
    out->op = cp_oem_auth_get_u16(in + 6u);
    out->request_id = cp_oem_auth_get_u32(in + 8u);
    out->payload_len = cp_oem_auth_get_u32(in + 12u);
    out->reserved = cp_oem_auth_get_u32(in + 16u);
    if (out->magic != CP_OEM_AUTH_RPC_MAGIC ||
        out->version != CP_OEM_AUTH_RPC_VERSION ||
        out->reserved != 0u ||
        out->payload_len > CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD) {
        return false;
    }
    if (out->op == CP_OEM_AUTH_RPC_OP_CERTIFICATE) {
        return out->payload_len == 0u;
    }
    if (out->op == CP_OEM_AUTH_RPC_OP_SIGNATURE) {
        return out->payload_len == CP_OEM_AUTH_CHALLENGE_SHA1 ||
               out->payload_len == CP_OEM_AUTH_CHALLENGE_SHA256;
    }
    return false;
}

static inline bool cp_oem_auth_rpc_response_decode(
    const uint8_t *in, size_t len, cp_oem_auth_rpc_response *out)
{
    if (in == NULL || out == NULL || len < CP_OEM_AUTH_RPC_RESPONSE_LEN) {
        return false;
    }
    out->magic = cp_oem_auth_get_u32(in);
    out->version = cp_oem_auth_get_u16(in + 4u);
    out->op = cp_oem_auth_get_u16(in + 6u);
    out->request_id = cp_oem_auth_get_u32(in + 8u);
    out->status = (int32_t)cp_oem_auth_get_u32(in + 12u);
    out->payload_len = cp_oem_auth_get_u32(in + 16u);
    out->reserved = cp_oem_auth_get_u32(in + 20u);
    if (out->magic != CP_OEM_AUTH_RPC_MAGIC ||
        out->version != CP_OEM_AUTH_RPC_VERSION ||
        out->reserved != 0u ||
        out->payload_len > CP_OEM_AUTH_RPC_MAX_FRAME -
                               CP_OEM_AUTH_RPC_RESPONSE_LEN) {
        return false;
    }
    return out->op == CP_OEM_AUTH_RPC_OP_CERTIFICATE ||
           out->op == CP_OEM_AUTH_RPC_OP_SIGNATURE;
}

/* ---------------------------------------------------------------- */
/* Native adapter helpers (shared with the mock tests and the helper) */
/* ---------------------------------------------------------------- */

static inline void cp_oem_auth_secure_zero(void *buffer, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buffer;
    while (len-- != 0u) {
        *p++ = 0u;
    }
}

static inline cp_oem_auth_error cp_oem_auth_map_backend_error(
    int rc, cp_oem_auth_error fallback)
{
    if (rc == 0) {
        return CP_OEM_AUTH_OK;
    }
    if (rc > 0 && rc <= (int)CP_OEM_AUTH_ERR_INTERNAL) {
        return (cp_oem_auth_error)rc;
    }
    return fallback;
}

static inline cp_oem_auth_error cp_oem_auth_native_initialize(
    const cp_oem_auth_native_ops *ops)
{
    if (ops == NULL || ops->initialize == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    return cp_oem_auth_map_backend_error(ops->initialize(ops->context),
                                         CP_OEM_AUTH_ERR_INIT);
}

static inline cp_oem_auth_error cp_oem_auth_native_certificate(
    const cp_oem_auth_native_ops *ops, uint8_t *out, size_t cap,
    size_t *out_len)
{
    void *buffer = NULL;
    size_t len = 0u;
    int rc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ops == NULL || out == NULL || out_len == NULL ||
        ops->copy_certificate == NULL || ops->release == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    rc = ops->copy_certificate(ops->context, &buffer, &len);
    if (rc != 0) {
        return cp_oem_auth_map_backend_error(rc, CP_OEM_AUTH_ERR_CERTIFICATE);
    }
    if (buffer == NULL || len == 0u) {
        if (buffer != NULL) {
            ops->release(ops->context, buffer, len);
        }
        return CP_OEM_AUTH_ERR_CERTIFICATE;
    }
    if (len > CP_OEM_AUTH_MAX_CERTIFICATE) {
        /* The length is not trustworthy enough to zero safely. */
        ops->release(ops->context, buffer, len);
        return CP_OEM_AUTH_ERR_CERTIFICATE;
    }
    if (cap < len) {
        cp_oem_auth_secure_zero(buffer, len);
        ops->release(ops->context, buffer, len);
        return CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL;
    }
    memcpy(out, buffer, len);
    cp_oem_auth_secure_zero(buffer, len);
    ops->release(ops->context, buffer, len);
    *out_len = len;
    return CP_OEM_AUTH_OK;
}

static inline cp_oem_auth_error cp_oem_auth_native_signature(
    const cp_oem_auth_native_ops *ops, const uint8_t *digest,
    size_t digest_len, uint8_t *out, size_t cap, size_t *out_len)
{
    void *buffer = NULL;
    size_t len = 0u;
    int rc;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (ops == NULL || digest == NULL || out == NULL || out_len == NULL ||
        ops->create_signature == NULL || ops->release == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (digest_len != CP_OEM_AUTH_CHALLENGE_SHA1 &&
        digest_len != CP_OEM_AUTH_CHALLENGE_SHA256) {
        return CP_OEM_AUTH_ERR_CHALLENGE_LENGTH;
    }
    rc = ops->create_signature(ops->context, digest, digest_len, &buffer,
                               &len);
    if (rc != 0) {
        return cp_oem_auth_map_backend_error(rc, CP_OEM_AUTH_ERR_SIGNATURE);
    }
    if (buffer == NULL || len == 0u) {
        if (buffer != NULL) {
            ops->release(ops->context, buffer, len);
        }
        return CP_OEM_AUTH_ERR_SIGNATURE;
    }
    if (len > CP_OEM_AUTH_MAX_SIGNATURE) {
        ops->release(ops->context, buffer, len);
        return CP_OEM_AUTH_ERR_SIGNATURE;
    }
    if (cap < len) {
        cp_oem_auth_secure_zero(buffer, len);
        ops->release(ops->context, buffer, len);
        return CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL;
    }
    memcpy(out, buffer, len);
    cp_oem_auth_secure_zero(buffer, len);
    ops->release(ops->context, buffer, len);
    *out_len = len;
    return CP_OEM_AUTH_OK;
}

/* ---------------------------------------------------------------- */
/* Public adapter API                                               */
/* ---------------------------------------------------------------- */

cp_oem_auth_error cp_oem_auth_open_rpc(cp_oem_auth *auth,
                                       const cp_oem_auth_rpc_config *config);
#ifdef CP_OEM_AUTH_TEST
cp_oem_auth_error cp_oem_auth_open_mock(cp_oem_auth *auth,
                                        const cp_oem_auth_native_ops *ops);
#endif
cp_boot_auth_provider cp_oem_auth_boot_provider(cp_oem_auth *auth);
int cp_oem_auth_provider_certificate(void *context, uint8_t *out, size_t cap,
                                     size_t *out_len);
int cp_oem_auth_provider_signature(void *context, const uint8_t *challenge,
                                   size_t challenge_len, uint8_t *out,
                                   size_t cap, size_t *out_len);
cp_oem_auth_error cp_oem_auth_last_error(cp_oem_auth *auth);
void cp_oem_auth_destroy(cp_oem_auth *auth);
const char *cp_oem_auth_error_name(cp_oem_auth_error error);

#ifdef __cplusplus
}
#endif

#endif /* CP_OEM_AUTH_H */
