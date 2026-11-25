/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline tests for the hardware-backed MFi adapter.  All providers are
 * first-party mocks; no OEM library is loaded or executed.  The only bytes
 * used for certificate/signature fixtures are invented test patterns.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "cp_bootstrap.h"
#include "cp_oem_auth.h"

static int failures;
static int checks;

#define CHECK(expr)                                                          \
    do {                                                                     \
        checks++;                                                            \
        if (!(expr)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long long a_ = (long long)(actual);                                  \
        long long e_ = (long long)(expected);                                \
        checks++;                                                            \
        if (a_ != e_) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s=%lld, expected %lld\n",         \
                    __FILE__, __LINE__, #actual, a_, e_);                    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

typedef struct {
    int init_rc;
    int certificate_rc;
    int signature_rc;
    bool certificate_null;
    bool signature_null;
    size_t certificate_len;
    size_t signature_len;
    int init_calls;
    int certificate_calls;
    int signature_calls;
    int release_calls;
    size_t last_digest_len;
    bool bad_digest_seen;
    bool release_zeroed;
    bool release_oversize;
    atomic_int active;
    atomic_int max_active;
} mock_state;

static void mock_sleep(void)
{
    struct timespec ts;

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000L;
    (void)nanosleep(&ts, NULL);
}

static void mock_enter(mock_state *mock)
{
    int active = atomic_fetch_add(&mock->active, 1) + 1;
    int maximum = atomic_load(&mock->max_active);

    while (active > maximum &&
           !atomic_compare_exchange_weak(&mock->max_active, &maximum,
                                         active)) {
    }
}

static void mock_leave(mock_state *mock)
{
    (void)atomic_fetch_sub(&mock->active, 1);
}

static int mock_initialize(void *context)
{
    mock_state *mock = (mock_state *)context;

    mock->init_calls++;
    mock_sleep();
    return mock->init_rc;
}

static int mock_certificate(void *context, void **out, size_t *out_len)
{
    mock_state *mock = (mock_state *)context;
    uint8_t *buffer;
    size_t i;

    mock->certificate_calls++;
    mock_enter(mock);
    mock_sleep();
    if (mock->certificate_rc != 0) {
        mock_leave(mock);
        return mock->certificate_rc;
    }
    if (mock->certificate_null) {
        *out = NULL;
        *out_len = 0u;
        mock_leave(mock);
        return 0;
    }
    buffer = (uint8_t *)malloc(mock->certificate_len == 0u
                                   ? 1u
                                   : mock->certificate_len);
    if (buffer == NULL) {
        mock_leave(mock);
        return CP_OEM_AUTH_ERR_OOM;
    }
    for (i = 0; i < mock->certificate_len; i++) {
        buffer[i] = (uint8_t)(0xA0u + (i & 0x1Fu));
    }
    *out = buffer;
    *out_len = mock->certificate_len;
    mock_leave(mock);
    return 0;
}

static int mock_signature(void *context, const void *digest,
                          size_t digest_len, void **out, size_t *out_len)
{
    mock_state *mock = (mock_state *)context;
    uint8_t *buffer;
    size_t i;

    (void)digest;
    mock->signature_calls++;
    mock->last_digest_len = digest_len;
    if (digest_len != CP_OEM_AUTH_CHALLENGE_SHA1 &&
        digest_len != CP_OEM_AUTH_CHALLENGE_SHA256) {
        mock->bad_digest_seen = true;
    }
    mock_enter(mock);
    mock_sleep();
    if (mock->signature_rc != 0) {
        mock_leave(mock);
        return mock->signature_rc;
    }
    if (mock->signature_null) {
        *out = NULL;
        *out_len = 0u;
        mock_leave(mock);
        return 0;
    }
    buffer = (uint8_t *)malloc(mock->signature_len == 0u
                                   ? 1u
                                   : mock->signature_len);
    if (buffer == NULL) {
        mock_leave(mock);
        return CP_OEM_AUTH_ERR_OOM;
    }
    for (i = 0; i < mock->signature_len; i++) {
        buffer[i] = (uint8_t)(0x5Au ^ (i & 0x0Fu));
    }
    *out = buffer;
    *out_len = mock->signature_len;
    mock_leave(mock);
    return 0;
}

static void mock_release(void *context, void *buffer, size_t len)
{
    mock_state *mock = (mock_state *)context;
    uint8_t *bytes = (uint8_t *)buffer;
    size_t i;
    bool zeroed = true;

    mock->release_calls++;
    if (buffer == NULL) {
        return;
    }
    if (len > CP_OEM_AUTH_MAX_CERTIFICATE) {
        mock->release_oversize = true;
        free(buffer);
        return;
    }
    for (i = 0; i < len; i++) {
        if (bytes[i] != 0u) {
            zeroed = false;
            break;
        }
    }
    mock->release_zeroed = zeroed;
    free(buffer);
}

static cp_oem_auth_native_ops mock_ops(mock_state *mock)
{
    cp_oem_auth_native_ops ops;

    memset(&ops, 0, sizeof(ops));
    ops.context = mock;
    ops.initialize = mock_initialize;
    ops.copy_certificate = mock_certificate;
    ops.create_signature = mock_signature;
    ops.release = mock_release;
    return ops;
}

static void mock_defaults(mock_state *mock)
{
    memset(mock, 0, sizeof(*mock));
    mock->certificate_len = 64u;
    mock->signature_len = 32u;
    atomic_init(&mock->active, 0);
    atomic_init(&mock->max_active, 0);
}

static bool bytes_equal_pattern(const uint8_t *data, size_t len,
                                uint8_t base, uint8_t mask)
{
    size_t i;

    for (i = 0; i < len; i++) {
        if (data[i] != (uint8_t)(base + (i & mask))) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- */
/* Provider adapter tests                                             */
/* ---------------------------------------------------------------- */

static void test_open_gate(void)
{
    cp_oem_auth auth;
    cp_oem_auth_rpc_config config;
    cp_boot_auth_provider provider;
    uint8_t out[64];
    size_t out_len = 99u;

    memset(&config, 0, sizeof(config));
    config.socket_path = "/tmp/opencode/nonexistent-auth.sock";
    config.experimental = false;
    CHECK_EQ(cp_oem_auth_open_rpc(&auth, &config), CP_OEM_AUTH_ERR_DISABLED);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_DISABLED);
    provider = cp_oem_auth_boot_provider(&auth);
    CHECK(provider.certificate != NULL);
    CHECK(provider.signature != NULL);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(out_len, 0u);
    cp_oem_auth_destroy(&auth);

    provider = cp_oem_auth_boot_provider(NULL);
    CHECK(provider.context == NULL);
    CHECK(provider.certificate == NULL);
    CHECK(provider.signature == NULL);

    config.experimental = true;
    config.socket_path = "relative.sock";
    CHECK_EQ(cp_oem_auth_open_rpc(&auth, &config), CP_OEM_AUTH_ERR_ARGUMENT);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_ARGUMENT);
    cp_oem_auth_destroy(&auth);

    config.socket_path = "/tmp/opencode/no-server.sock";
    config.timeout_ms = CP_OEM_AUTH_RPC_MAX_TIMEOUT_MS + 1u;
    CHECK_EQ(cp_oem_auth_open_rpc(&auth, &config), CP_OEM_AUTH_ERR_ARGUMENT);
    cp_oem_auth_destroy(&auth);
}

static void test_mock_certificate_success(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    cp_boot_auth_provider provider;
    uint8_t out[96];
    uint8_t canary[114];
    size_t out_len = 0u;

    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    provider = cp_oem_auth_boot_provider(&auth);
    CHECK(provider.certificate != NULL);

    memset(out, 0xCC, sizeof(out));
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             0);
    CHECK_EQ(out_len, 64u);
    CHECK(bytes_equal_pattern(out, 64u, 0xA0u, 0x1Fu));
    CHECK_EQ(mock.init_calls, 1);
    CHECK_EQ(mock.certificate_calls, 1);
    CHECK_EQ(mock.release_calls, 1);
    CHECK(mock.release_zeroed);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_OK);

    memset(canary, 0x3C, sizeof(canary));
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, canary + 1, 64u,
                                              &out_len),
             0);
    CHECK_EQ(out_len, 64u);
    CHECK_EQ(canary[0], 0x3Cu);
    CHECK_EQ(canary[65], 0x3Cu);
    CHECK(bytes_equal_pattern(canary + 1, 64u, 0xA0u, 0x1Fu));
    cp_oem_auth_destroy(&auth);
}

static void test_mock_certificate_failures(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    uint8_t out[128];
    uint8_t canary[18];
    uint8_t challenge[20] = {0};
    size_t out_len = 123u;

    mock_defaults(&mock);
    mock.init_rc = CP_OEM_AUTH_ERR_INIT;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_INIT);
    CHECK_EQ(mock.init_calls, 1);
    CHECK_EQ(mock.certificate_calls, 0);

    /* A failed backend initialization is latched: neither certificate nor
     * signature may reach the provider again. */
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge,
                                            sizeof(challenge), out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_INIT);
    CHECK_EQ(mock.signature_calls, 0);
    CHECK_EQ(mock.init_calls, 1);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_INIT);
    CHECK_EQ(mock.certificate_calls, 0);
    CHECK_EQ(mock.init_calls, 1);
    cp_oem_auth_destroy(&auth);

    /* Init returns 0, but the actual certificate call fails. */
    mock_defaults(&mock);
    mock.certificate_rc = CP_OEM_AUTH_ERR_CERTIFICATE;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_CERTIFICATE);
    CHECK_EQ(mock.init_calls, 1);
    CHECK_EQ(mock.certificate_calls, 1);
    cp_oem_auth_destroy(&auth);

    mock_defaults(&mock);
    mock.certificate_null = true;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_CERTIFICATE);
    cp_oem_auth_destroy(&auth);

    mock_defaults(&mock);
    mock.certificate_len = 0u;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_CERTIFICATE);
    cp_oem_auth_destroy(&auth);

    mock_defaults(&mock);
    mock.certificate_rc = CP_OEM_AUTH_ERR_OOM;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_OOM);
    cp_oem_auth_destroy(&auth);

    /* Buffer overflow: the caller buffer is untouched and the OEM copy is
     * zeroed before release. */
    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    memset(canary, 0x6D, sizeof(canary));
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, canary + 1, 8u,
                                              &out_len),
             -1);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(cp_oem_auth_last_error(&auth),
             CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL);
    CHECK_EQ(canary[0], 0x6Du);
    CHECK_EQ(canary[9], 0x6Du);
    CHECK_EQ(canary[17], 0x6Du);
    CHECK(mock.release_zeroed);
    cp_oem_auth_destroy(&auth);

    /* A backend that lies about a huge length must not be zeroed blindly. */
    mock_defaults(&mock);
    mock.certificate_len = CP_OEM_AUTH_MAX_CERTIFICATE + 1u;
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_CERTIFICATE);
    CHECK(mock.release_oversize);
    cp_oem_auth_destroy(&auth);
}

static void test_mock_signature(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    uint8_t challenge[32];
    uint8_t out[64];
    uint8_t canary[42];
    size_t out_len = 0u;
    size_t i;
    static const size_t bad_lengths[] = {0u, 1u, 19u, 21u, 31u, 33u, 64u};

    for (i = 0; i < sizeof(challenge); i++) {
        challenge[i] = (uint8_t)i;
    }

    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             0);
    CHECK_EQ(out_len, 32u);
    CHECK_EQ(mock.last_digest_len, 20u);
    CHECK_EQ(mock.signature_calls, 1);
    CHECK(mock.release_zeroed);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_OK);
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 32u, out,
                                            sizeof(out), &out_len),
             0);
    CHECK_EQ(out_len, 32u);
    CHECK_EQ(mock.last_digest_len, 32u);

    for (i = 0; i < sizeof(bad_lengths) / sizeof(bad_lengths[0]); i++) {
        size_t before = (size_t)mock.signature_calls;
        CHECK_EQ(cp_oem_auth_provider_signature(
                     &auth, challenge, bad_lengths[i], out, sizeof(out),
                     &out_len),
                 -1);
        CHECK_EQ(out_len, 0u);
        CHECK_EQ(cp_oem_auth_last_error(&auth),
                 CP_OEM_AUTH_ERR_CHALLENGE_LENGTH);
        CHECK_EQ((size_t)mock.signature_calls, before);
    }
    CHECK(!mock.bad_digest_seen);

    mock.signature_rc = CP_OEM_AUTH_ERR_SIGNATURE;
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_SIGNATURE);

    mock.signature_rc = CP_OEM_AUTH_ERR_OOM;
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_OOM);

    mock.signature_rc = 0;
    mock.signature_null = true;
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_SIGNATURE);

    mock.signature_null = false;
    mock.signature_len = 0u;
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_SIGNATURE);

    mock.signature_len = CP_OEM_AUTH_MAX_SIGNATURE + 1u;
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u, out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_SIGNATURE);

    mock.signature_len = 32u;
    memset(canary, 0x7E, sizeof(canary));
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge, 20u,
                                            canary + 1, 8u, &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth),
             CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL);
    CHECK_EQ(canary[0], 0x7Eu);
    CHECK_EQ(canary[9], 0x7Eu);
    CHECK_EQ(canary[41], 0x7Eu);
    CHECK(mock.release_zeroed);
    cp_oem_auth_destroy(&auth);
}

typedef struct {
    cp_oem_auth *auth;
    const uint8_t *challenge;
    size_t challenge_len;
    int failures;
} thread_args;

static void *thread_certificate(void *context)
{
    thread_args *args = (thread_args *)context;
    int i;

    for (i = 0; i < 40; i++) {
        uint8_t out[64];
        size_t out_len = 0u;
        if (cp_oem_auth_provider_certificate(args->auth, out, sizeof(out),
                                             &out_len) != 0 ||
            out_len != 64u) {
            args->failures++;
        }
    }
    for (i = 0; i < 40; i++) {
        uint8_t out[64];
        size_t out_len = 0u;
        if (cp_oem_auth_provider_signature(args->auth, args->challenge,
                                           args->challenge_len, out,
                                           sizeof(out), &out_len) != 0 ||
            out_len != 32u) {
            args->failures++;
        }
    }
    return NULL;
}

static void test_serialization(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    pthread_t threads[2];
    thread_args args[2];
    uint8_t challenge[20];
    size_t i;

    for (i = 0; i < sizeof(challenge); i++) {
        challenge[i] = (uint8_t)i;
    }
    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    for (i = 0; i < 2u; i++) {
        args[i].auth = &auth;
        args[i].challenge = challenge;
        args[i].challenge_len = sizeof(challenge);
        args[i].failures = 0;
        CHECK_EQ(pthread_create(&threads[i], NULL, thread_certificate,
                                &args[i]),
                 0);
    }
    for (i = 0; i < 2u; i++) {
        CHECK_EQ(pthread_join(threads[i], NULL), 0);
        CHECK_EQ(args[i].failures, 0);
    }
    CHECK_EQ(atomic_load(&mock.max_active), 1);
    CHECK_EQ(mock.certificate_calls, 80);
    CHECK_EQ(mock.signature_calls, 80);
    cp_oem_auth_destroy(&auth);
}

static void test_teardown_zeroization(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    uint8_t *bytes;
    size_t i;
    bool all_zero = true;

    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    memset(auth.rx, 0xA5, sizeof(auth.rx));
    memset(auth.tx, 0x5A, sizeof(auth.tx));
    memset(auth.socket_path, 0x3C, sizeof(auth.socket_path));
    cp_oem_auth_destroy(&auth);
    bytes = (uint8_t *)&auth;
    for (i = 0; i < sizeof(auth); i++) {
        if (bytes[i] != 0u) {
            all_zero = false;
            break;
        }
    }
    CHECK(all_zero);
    cp_oem_auth_destroy(&auth); /* second destroy is explicitly safe */
}

static void test_destroy_lock_failure_closes_fd(void)
{
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    int fd;

    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    fd = dup(STDOUT_FILENO);
    CHECK(fd >= 0);
    auth.fd = fd;
    auth.force_destroy_lock_failure = true;
    cp_oem_auth_destroy(&auth);
    errno = 0;
    CHECK_EQ(fcntl(fd, F_GETFD), -1);
    CHECK_EQ(errno, EBADF);
}

static void test_rpc_codec(void)
{
    cp_oem_auth_rpc_request request;
    cp_oem_auth_rpc_response response;
    uint8_t request_bytes[CP_OEM_AUTH_RPC_REQUEST_LEN];
    uint8_t response_bytes[CP_OEM_AUTH_RPC_RESPONSE_LEN];
    cp_oem_auth_rpc_request decoded_request;
    cp_oem_auth_rpc_response decoded_response;

    memset(&request, 0, sizeof(request));
    request.magic = CP_OEM_AUTH_RPC_MAGIC;
    request.version = CP_OEM_AUTH_RPC_VERSION;
    request.op = CP_OEM_AUTH_RPC_OP_SIGNATURE;
    request.request_id = 0x01020304u;
    request.payload_len = 20u;
    cp_oem_auth_rpc_request_encode(request_bytes, &request);
    CHECK(request_bytes[0] == 0x53u && request_bytes[1] == 0x43u &&
          request_bytes[2] == 0x41u && request_bytes[3] == 0x31u);
    CHECK_EQ(request_bytes[7], 2u);
    CHECK_EQ(request_bytes[8], 1u);
    CHECK_EQ(request_bytes[11], 4u);
    CHECK_EQ(request_bytes[15], 20u);
    CHECK(cp_oem_auth_rpc_request_decode(request_bytes, sizeof(request_bytes),
                                         &decoded_request));
    CHECK_EQ(decoded_request.request_id, 0x01020304u);
    CHECK_EQ(decoded_request.payload_len, 20u);

    request_bytes[0] = 0x00u;
    CHECK(!cp_oem_auth_rpc_request_decode(request_bytes,
                                          sizeof(request_bytes),
                                          &decoded_request));
    request_bytes[0] = 0x53u;
    request_bytes[4] = 2u;
    CHECK(!cp_oem_auth_rpc_request_decode(request_bytes,
                                          sizeof(request_bytes),
                                          &decoded_request));
    request_bytes[4] = 0u;
    request_bytes[6] = 9u;
    CHECK(!cp_oem_auth_rpc_request_decode(request_bytes,
                                          sizeof(request_bytes),
                                          &decoded_request));
    request_bytes[6] = 0u;
    request_bytes[7] = 2u;
    request_bytes[12] = 0u;
    request_bytes[13] = 0u;
    request_bytes[14] = 0u;
    request_bytes[15] = 19u;
    CHECK(!cp_oem_auth_rpc_request_decode(request_bytes,
                                          sizeof(request_bytes),
                                          &decoded_request));
    CHECK(!cp_oem_auth_rpc_request_decode(request_bytes,
                                          sizeof(request_bytes) - 1u,
                                          &decoded_request));

    memset(&response, 0, sizeof(response));
    response.magic = CP_OEM_AUTH_RPC_MAGIC;
    response.version = CP_OEM_AUTH_RPC_VERSION;
    response.op = CP_OEM_AUTH_RPC_OP_CERTIFICATE;
    response.request_id = 7u;
    response.status = CP_OEM_AUTH_OK;
    response.payload_len = 64u;
    cp_oem_auth_rpc_response_encode(response_bytes, &response);
    CHECK_EQ(response_bytes[19], 64u);
    CHECK(cp_oem_auth_rpc_response_decode(response_bytes,
                                          sizeof(response_bytes),
                                          &decoded_response));
    CHECK_EQ(decoded_response.payload_len, 64u);
    CHECK_EQ(decoded_response.status, CP_OEM_AUTH_OK);
    response_bytes[16] = 0xFFu;
    response_bytes[17] = 0xFFu;
    response_bytes[18] = 0xFFu;
    response_bytes[19] = 0xFFu;
    CHECK(!cp_oem_auth_rpc_response_decode(response_bytes,
                                           sizeof(response_bytes),
                                           &decoded_response));
    response_bytes[16] = 0u;
    response_bytes[17] = 0u;
    response_bytes[18] = 0u;
    response_bytes[19] = 0u;
    response_bytes[6] = 9u;
    CHECK(!cp_oem_auth_rpc_response_decode(response_bytes,
                                           sizeof(response_bytes),
                                           &decoded_response));
}

static void test_rpc_no_server(void)
{
    cp_oem_auth auth;
    cp_oem_auth_rpc_config config;
    cp_boot_auth_provider provider;
    uint8_t out[64];
    uint8_t challenge[20];
    size_t out_len = 0u;
    char dir[] = "/tmp/opencode/cp-auth-noserver-XXXXXX";
    char socket_path[CP_OEM_AUTH_SOCKET_PATH_MAX];

    CHECK(mkdtemp(dir) != NULL);
    CHECK(snprintf(socket_path, sizeof(socket_path), "%s/auth.sock", dir) > 0);
    memset(&config, 0, sizeof(config));
    config.socket_path = socket_path;
    config.timeout_ms = 100u;
    config.experimental = true;
    config.expected_peer_uid = (uint32_t)geteuid();
    CHECK_EQ(cp_oem_auth_open_rpc(&auth, &config), CP_OEM_AUTH_OK);
    provider = cp_oem_auth_boot_provider(&auth);
    CHECK(provider.certificate != NULL);
    CHECK_EQ(cp_oem_auth_provider_certificate(&auth, out, sizeof(out),
                                              &out_len),
             -1);
    CHECK_EQ(out_len, 0u);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_RPC);
    memset(challenge, 0x11, sizeof(challenge));
    CHECK_EQ(cp_oem_auth_provider_signature(&auth, challenge,
                                            sizeof(challenge), out,
                                            sizeof(out), &out_len),
             -1);
    CHECK_EQ(cp_oem_auth_last_error(&auth), CP_OEM_AUTH_ERR_RPC);
    cp_oem_auth_destroy(&auth);
    CHECK_EQ(rmdir(dir), 0);
}

/* ---------------------------------------------------------------- */
/* No-bypass integration with cp_bootstrap                            */
/* ---------------------------------------------------------------- */

static size_t unhex(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = strlen(hex);
    size_t i;

    if ((n % 2u) != 0u || n / 2u > cap) {
        return 0u;
    }
    for (i = 0; i < n / 2u; i++) {
        unsigned int value = 0u;
        size_t j;
        for (j = 0; j < 2u; j++) {
            char c = hex[2u * i + j];
            unsigned int digit;
            if (c >= '0' && c <= '9') {
                digit = (unsigned int)(c - '0');
            } else if (c >= 'a' && c <= 'f') {
                digit = (unsigned int)(c - 'a' + 10);
            } else if (c >= 'A' && c <= 'F') {
                digit = (unsigned int)(c - 'A' + 10);
            } else {
                return 0u;
            }
            value = (value << 4) | digit;
        }
        out[i] = (uint8_t)value;
    }
    return n / 2u;
}

static cp_boot_result feed_frame(cp_bootstrap *session, const char *hex,
                                 uint64_t now, uint8_t *out, size_t cap)
{
    uint8_t frame[256];
    size_t len = unhex(hex, frame, sizeof(frame));
    cp_boot_result result;

    memset(&result, 0, sizeof(result));
    if (len == 0u) {
        result.event = CP_BOOT_EV_ERROR;
        result.error = CP_BOOT_ERR_ARGUMENT;
        return result;
    }
    return cp_bootstrap_receive(session, frame, len, now, out, cap);
}

static void test_no_auth_bypass(void)
{
    static const char *const languages[] = {"en"};
    cp_bootstrap session;
    cp_boot_config config;
    cp_oem_auth auth;
    mock_state mock;
    cp_oem_auth_native_ops ops;
    cp_boot_auth_provider provider;
    uint8_t out[512];
    uint64_t now = 1u;
    cp_boot_result result;

    memset(&config, 0, sizeof(config));
    config.name = "SCOURGE";
    config.model_identifier = "GEN5W";
    config.manufacturer = "SCOURGE";
    config.serial_number = "SN-0001";
    config.firmware_version = "1.0.0";
    config.hardware_version = "1.0";
    config.eap_name = "com.scourge.carplay";
    config.current_language = "en";
    config.supported_languages = languages;
    config.supported_language_count = 1u;
    config.app_match_team_id = NULL;
    config.bluetooth_transport_name = "bt";
    config.bluetooth_mac = "AA:BB:CC:DD:EE:FF";
    config.maximum_current_drawn_ma = 0u;
    config.ap_ssid = "SCOURGE-AP";
    config.ap_passphrase = "Wireless-Test-1";
    config.ap_channel = 36u;
    config.ap_security = CP_WIRE_WIFI_WPA2;
    config.link_local = "fe80::1";
    config.airplay_port = 7000u;
    config.device_identifier = "SCOURGE-DEV";
    config.public_key = "test-public-key";
    config.source_version = "1.0.0";
    config.ap_wait_ms = 1000u;

    mock_defaults(&mock);
    ops = mock_ops(&mock);
    CHECK_EQ(cp_oem_auth_open_mock(&auth, &ops), CP_OEM_AUTH_OK);
    provider = cp_oem_auth_boot_provider(&auth);
    CHECK_EQ(cp_bootstrap_init(&session, &config), CP_BOOT_OK);
    CHECK_EQ(cp_bootstrap_set_auth_provider(&session, &provider), CP_BOOT_OK);

    result = feed_frame(&session, "404000061d00", now++, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFICATION_SENT);
    result = feed_frame(&session, "404000061d02", now++, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_IDENTIFIED);
    result = feed_frame(&session, "40400006aa00", now++, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_CERT_SENT);
    CHECK_EQ(mock.certificate_calls, 1);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_CERT_SENT);
    result = feed_frame(
        &session,
        "4040001eaa0200180000000102030405060708090a0b0c0d0e0f1011121314",
        now++, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTH_RESPONSE_SENT);
    CHECK_EQ(mock.signature_calls, 1);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_RESPONSE_SENT);
    CHECK(cp_bootstrap_auth_state(&session) != CP_BOOT_AUTH_SUCCEEDED);
    result = feed_frame(&session, "40400006aa05", now++, out, sizeof(out));
    CHECK_EQ(result.event, CP_BOOT_EV_AUTHENTICATED);
    CHECK_EQ(cp_bootstrap_auth_state(&session), CP_BOOT_AUTH_SUCCEEDED);
    cp_oem_auth_destroy(&auth);
}

/* ---------------------------------------------------------------- */
/* Optional RPC client mode used by the Python transport test         */
/* ---------------------------------------------------------------- */

static int rpc_client_run(const char *socket_path, const char *mode,
                          uint32_t timeout_ms, size_t out_cap)
{
    cp_oem_auth_rpc_config config;
    cp_oem_auth auth;
    cp_boot_auth_provider provider;
    uint8_t out[CP_OEM_AUTH_MAX_CERTIFICATE];
    uint8_t challenge[20];
    size_t out_len = 0u;
    size_t i;
    int rc;
    int cert_rc = 0;
    int signature_rc = 0;
    int cert_error = 0;
    int signature_error = 0;

    memset(&config, 0, sizeof(config));
    config.socket_path = socket_path;
    config.timeout_ms = timeout_ms == 0u ? 1000u : timeout_ms;
    config.experimental = true;
    config.expected_peer_uid = (uint32_t)geteuid();
    rc = cp_oem_auth_open_rpc(&auth, &config);
    if (rc != CP_OEM_AUTH_OK) {
        printf("open_error=%d\n", (int)rc);
        return (int)rc;
    }
    provider = cp_oem_auth_boot_provider(&auth);
    memset(challenge, 0x42, sizeof(challenge));
    if (out_cap == 0u || out_cap > sizeof(out)) {
        out_cap = sizeof(out);
    }
    if (strcmp(mode, "sign") != 0) {
        cert_rc = cp_oem_auth_provider_certificate(
            provider.context, out, out_cap, &out_len);
        if (cert_rc == 0) {
            printf("cert_len=%zu\n", out_len);
            printf("cert_hex=");
            for (i = 0; i < out_len; i++) {
                printf("%02x", out[i]);
            }
            putchar('\n');
        } else {
            cert_error = (int)cp_oem_auth_last_error(&auth);
            printf("cert_error=%d\n", cert_error);
        }
    }
    if (strcmp(mode, "cert") != 0) {
        out_len = 0u;
        signature_rc = cp_oem_auth_provider_signature(
            provider.context, challenge, sizeof(challenge), out, out_cap,
            &out_len);
        if (signature_rc == 0) {
            printf("sig_len=%zu\n", out_len);
            printf("sig_hex=");
            for (i = 0; i < out_len; i++) {
                printf("%02x", out[i]);
            }
            putchar('\n');
        } else {
            signature_error = (int)cp_oem_auth_last_error(&auth);
            printf("sig_error=%d\n", signature_error);
        }
    }
    if (cert_rc != 0) {
        rc = cert_error;
    } else if (signature_rc != 0) {
        rc = signature_error;
    } else {
        rc = 0;
    }
    cp_oem_auth_destroy(&auth);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "--rpc-client") == 0) {
        char *end = NULL;
        unsigned long timeout = 0u;
        unsigned long cap = 0u;
        if (argc >= 5) {
            timeout = strtoul(argv[4], &end, 10);
            if (end == argv[4] || *end != '\0' || timeout > 60000u) {
                return 2;
            }
        }
        if (argc >= 6) {
            cap = strtoul(argv[5], &end, 10);
            if (end == argv[5] || *end != '\0' || cap == 0u ||
                cap > CP_OEM_AUTH_MAX_CERTIFICATE) {
                return 2;
            }
        }
        return rpc_client_run(argv[2], argc >= 4 ? argv[3] : "both",
                              (uint32_t)timeout, (size_t)cap);
    }
    if (argc != 1) {
        return 2;
    }

    test_open_gate();
    test_mock_certificate_success();
    test_mock_certificate_failures();
    test_mock_signature();
    test_serialization();
    test_teardown_zeroization();
    test_destroy_lock_failure_closes_fd();
    test_rpc_codec();
    test_rpc_no_server();
    test_no_auth_bypass();

    if (failures != 0) {
        fprintf(stderr, "%d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("carplay oem auth: %d checks passed\n", checks);
    return 0;
}
