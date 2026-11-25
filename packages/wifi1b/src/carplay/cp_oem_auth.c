/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * In-process side of the hardware-backed MFi provider.  Production talks to
 * the root helper over a bounded SOCK_SEQPACKET RPC; it never loads OEM code,
 * never opens I2C and never logs certificates or signatures.
 */

#define _GNU_SOURCE
#include "cp_oem_auth.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static size_t auth_strlen(const char *text, size_t limit)
{
    size_t n = 0u;

    if (text == NULL) {
        return limit + 1u;
    }
    while (n <= limit && text[n] != '\0') {
        n++;
    }
    return n;
}

static int auth_lstat(const char *path, struct stat *st)
{
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st,
                        AT_SYMLINK_NOFOLLOW);
}

static bool auth_parent_dir(const char *path, char *out, size_t cap)
{
    const char *slash;
    size_t n;

    if (path == NULL || out == NULL || cap == 0u) {
        return false;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        return false;
    }
    n = (size_t)(slash - path);
    if (n == 0u) {
        n = 1u; /* root */
    }
    if (n + 1u > cap) {
        return false;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

static uint32_t auth_expected_uid(const cp_oem_auth *auth)
{
#ifdef CP_OEM_AUTH_TEST
    return auth->expected_peer_uid;
#else
    (void)auth;
    return 0u;
#endif
}

static cp_oem_auth_error auth_check_socket(const cp_oem_auth *auth,
                                           const char *path)
{
    struct stat socket_stat;
    struct stat dir_stat;
    char parent[CP_OEM_AUTH_SOCKET_PATH_MAX];
    uint32_t uid = auth_expected_uid(auth);

    if (auth_lstat(path, &socket_stat) != 0) {
        return CP_OEM_AUTH_ERR_RPC;
    }
    if (!S_ISSOCK(socket_stat.st_mode) ||
        (uint32_t)socket_stat.st_uid != uid ||
        (socket_stat.st_mode & 0777u) != 0600u) {
        return CP_OEM_AUTH_ERR_RPC_PEER;
    }
    if (!auth_parent_dir(path, parent, sizeof(parent))) {
        return CP_OEM_AUTH_ERR_RPC_PEER;
    }
    if (auth_lstat(parent, &dir_stat) != 0) {
        return CP_OEM_AUTH_ERR_RPC;
    }
    if (!S_ISDIR(dir_stat.st_mode) || (uint32_t)dir_stat.st_uid != uid ||
        (dir_stat.st_mode & 0777u) != 0700u) {
        return CP_OEM_AUTH_ERR_RPC_PEER;
    }
    return CP_OEM_AUTH_OK;
}

static uint64_t auth_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0u;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static bool auth_deadline_reached(uint64_t deadline)
{
    return auth_now_ms() >= deadline;
}

static int auth_remaining_ms(uint64_t deadline)
{
    uint64_t now = auth_now_ms();
    uint64_t left;

    if (now >= deadline) {
        return 0;
    }
    left = deadline - now;
    if (left > (uint64_t)INT_MAX) {
        return INT_MAX;
    }
    return (int)left;
}

static bool auth_wait_fd(int fd, short events, uint64_t deadline)
{
    for (;;) {
        struct pollfd pfd;
        int rc;

        pfd.fd = fd;
        pfd.events = events;
        pfd.revents = 0;
        rc = poll(&pfd, 1, auth_remaining_ms(deadline));
        if (rc > 0) {
            return true;
        }
        if (rc == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        return false;
    }
}

static void auth_close_fd(cp_oem_auth *auth)
{
    if (auth->fd >= 0) {
        (void)close(auth->fd);
        auth->fd = -1;
    }
}

static cp_oem_auth_error auth_connect_locked(cp_oem_auth *auth,
                                             uint64_t deadline)
{
    struct sockaddr_un addr;
    struct ucred cred;
    socklen_t cred_len = (socklen_t)sizeof(cred);
    int fd;
    int flags;
    int so_error = 0;
    socklen_t so_len = (socklen_t)sizeof(so_error);
    cp_oem_auth_error err;

    if (auth->fd >= 0) {
        return CP_OEM_AUTH_OK;
    }
    if (auth_deadline_reached(deadline)) {
        return CP_OEM_AUTH_ERR_RPC_TIMEOUT;
    }
    err = auth_check_socket(auth, auth->socket_path);
    if (err != CP_OEM_AUTH_OK) {
        return err;
    }
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return CP_OEM_AUTH_ERR_RPC;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_RPC;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, auth->socket_path, strlen(auth->socket_path) + 1u);

    for (;;) {
        int rc;

        if (auth_deadline_reached(deadline)) {
            (void)close(fd);
            return CP_OEM_AUTH_ERR_RPC_TIMEOUT;
        }
        rc = connect(fd, (const struct sockaddr *)&addr, sizeof(addr));
        if (rc == 0 || (rc != 0 && errno == EISCONN)) {
            break;
        }
        if (errno != EINPROGRESS && errno != EALREADY && errno != EINTR &&
            errno != EAGAIN && errno != EWOULDBLOCK) {
            (void)close(fd);
            return CP_OEM_AUTH_ERR_RPC;
        }
        if (!auth_wait_fd(fd, POLLOUT, deadline)) {
            (void)close(fd);
            return CP_OEM_AUTH_ERR_RPC_TIMEOUT;
        }
        so_error = 0;
        so_len = (socklen_t)sizeof(so_error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 ||
            so_len != (socklen_t)sizeof(so_error) || so_error != 0) {
            (void)close(fd);
            return CP_OEM_AUTH_ERR_RPC;
        }
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred_len != (socklen_t)sizeof(cred) ||
        (uint32_t)cred.uid != auth_expected_uid(auth)) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_RPC_PEER;
    }
    auth->fd = fd;
    return CP_OEM_AUTH_OK;
}

static cp_oem_auth_error auth_send_record(cp_oem_auth *auth,
                                          const uint8_t *data, size_t len,
                                          uint64_t deadline)
{
    for (;;) {
        ssize_t sent;

        if (auth_deadline_reached(deadline)) {
            return CP_OEM_AUTH_ERR_RPC_TIMEOUT;
        }
        if (!auth_wait_fd(auth->fd, POLLOUT, deadline)) {
            return CP_OEM_AUTH_ERR_RPC_TIMEOUT;
        }
        sent = send(auth->fd, data, len, MSG_NOSIGNAL);
        if (sent == (ssize_t)len) {
            return CP_OEM_AUTH_OK;
        }
        if (sent < 0 &&
            (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
            continue;
        }
        return CP_OEM_AUTH_ERR_RPC;
    }
}

static ssize_t auth_recv_record(cp_oem_auth *auth, uint64_t deadline,
                               cp_oem_auth_error *error)
{
    for (;;) {
        ssize_t received;

        if (auth_deadline_reached(deadline)) {
            *error = CP_OEM_AUTH_ERR_RPC_TIMEOUT;
            return -1;
        }
        if (!auth_wait_fd(auth->fd, POLLIN, deadline)) {
            *error = CP_OEM_AUTH_ERR_RPC_TIMEOUT;
            return -1;
        }
        received = recv(auth->fd, auth->rx, sizeof(auth->rx), MSG_TRUNC);
        if (received > 0) {
            *error = CP_OEM_AUTH_OK;
            return received;
        }
        if (received == 0) {
            *error = CP_OEM_AUTH_ERR_RPC;
            return 0;
        }
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        *error = CP_OEM_AUTH_ERR_RPC;
        return -1;
    }
}

static cp_oem_auth_error auth_rpc_exchange_locked(
    cp_oem_auth *auth, uint16_t op, const uint8_t *payload, size_t payload_len,
    uint8_t *out, size_t cap, size_t *out_len)
{
    cp_oem_auth_rpc_request request;
    cp_oem_auth_rpc_response response;
    uint64_t deadline;
    uint32_t request_id;
    ssize_t received;
    size_t record_len;
    cp_oem_auth_error err;

    if (out_len == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    *out_len = 0u;
    if (payload_len > CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (op == CP_OEM_AUTH_RPC_OP_CERTIFICATE && payload_len != 0u) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (op == CP_OEM_AUTH_RPC_OP_SIGNATURE && payload_len !=
            CP_OEM_AUTH_CHALLENGE_SHA1 &&
        payload_len != CP_OEM_AUTH_CHALLENGE_SHA256) {
        return CP_OEM_AUTH_ERR_CHALLENGE_LENGTH;
    }
    if (payload_len != 0u && payload == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }

    deadline = auth_now_ms() + (uint64_t)auth->timeout_ms;
    err = auth_connect_locked(auth, deadline);
    if (err != CP_OEM_AUTH_OK) {
        return err;
    }

    request_id = auth->next_request_id++;
    if (request_id == 0u) {
        request_id = auth->next_request_id++;
    }
    memset(&request, 0, sizeof(request));
    request.magic = CP_OEM_AUTH_RPC_MAGIC;
    request.version = CP_OEM_AUTH_RPC_VERSION;
    request.op = op;
    request.request_id = request_id;
    request.payload_len = (uint32_t)payload_len;
    memset(auth->tx, 0, sizeof(auth->tx));
    cp_oem_auth_rpc_request_encode(auth->tx, &request);
    if (payload_len != 0u) {
        memcpy(auth->tx + CP_OEM_AUTH_RPC_REQUEST_LEN, payload, payload_len);
    }

    err = auth_send_record(auth, auth->tx,
                           CP_OEM_AUTH_RPC_REQUEST_LEN + payload_len,
                           deadline);
    if (err != CP_OEM_AUTH_OK) {
        auth_close_fd(auth);
        return err;
    }

    {
        cp_oem_auth_error recv_error = CP_OEM_AUTH_OK;
        received = auth_recv_record(auth, deadline, &recv_error);
        if (received < 0) {
            auth_close_fd(auth);
            return recv_error;
        }
    }
    if (received == 0) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC;
    }
    record_len = (size_t)received;
    if (record_len < CP_OEM_AUTH_RPC_RESPONSE_LEN ||
        record_len > sizeof(auth->rx)) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
    }
    if (!cp_oem_auth_rpc_response_decode(auth->rx, record_len, &response) ||
        response.request_id != request_id || response.op != op) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
    }
    if (response.status != 0) {
        if (response.payload_len != 0u ||
            record_len != CP_OEM_AUTH_RPC_RESPONSE_LEN ||
            response.status < 0 ||
            response.status > (int32_t)CP_OEM_AUTH_ERR_INTERNAL) {
            auth_close_fd(auth);
            return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
        }
        cp_oem_auth_secure_zero(auth->rx, sizeof(auth->rx));
        return (cp_oem_auth_error)response.status;
    }
    if (response.payload_len == 0u ||
        response.payload_len > sizeof(auth->rx) -
                                   CP_OEM_AUTH_RPC_RESPONSE_LEN ||
        record_len != CP_OEM_AUTH_RPC_RESPONSE_LEN + response.payload_len) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
    }
    if (op == CP_OEM_AUTH_RPC_OP_CERTIFICATE &&
        response.payload_len > CP_OEM_AUTH_MAX_CERTIFICATE) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
    }
    if (op == CP_OEM_AUTH_RPC_OP_SIGNATURE &&
        response.payload_len > CP_OEM_AUTH_MAX_SIGNATURE) {
        auth_close_fd(auth);
        return CP_OEM_AUTH_ERR_RPC_PROTOCOL;
    }
    if (cap < response.payload_len) {
        cp_oem_auth_secure_zero(auth->rx, sizeof(auth->rx));
        return CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL;
    }
    memcpy(out, auth->rx + CP_OEM_AUTH_RPC_RESPONSE_LEN,
           response.payload_len);
    *out_len = response.payload_len;
    cp_oem_auth_secure_zero(auth->rx, sizeof(auth->rx));
    return CP_OEM_AUTH_OK;
}

static cp_oem_auth_error auth_init_common(cp_oem_auth *auth)
{
    memset(auth, 0, sizeof(*auth));
    if (pthread_mutex_init(&auth->lock, NULL) != 0) {
        memset(auth, 0, sizeof(*auth));
        auth->last_error = CP_OEM_AUTH_ERR_INTERNAL;
        return CP_OEM_AUTH_ERR_INTERNAL;
    }
    auth->initialized = true;
    auth->fd = -1;
    auth->next_request_id = 1u;
    auth->last_error = CP_OEM_AUTH_OK;
    return CP_OEM_AUTH_OK;
}

static cp_oem_auth_error auth_open_disabled(cp_oem_auth *auth,
                                            cp_oem_auth_error error)
{
    memset(auth, 0, sizeof(*auth));
    auth->last_error = error;
    return error;
}

cp_oem_auth_error cp_oem_auth_open_rpc(cp_oem_auth *auth,
                                       const cp_oem_auth_rpc_config *config)
{
    const char *path;
    size_t path_len;
    cp_oem_auth_error err;

    if (auth == NULL || config == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (!config->experimental) {
        return auth_open_disabled(auth, CP_OEM_AUTH_ERR_DISABLED);
    }
    path = config->socket_path != NULL ? config->socket_path
                                       : CP_OEM_AUTH_RPC_DEFAULT_SOCKET;
    path_len = auth_strlen(path, CP_OEM_AUTH_SOCKET_PATH_MAX);
    if (path_len == 0u || path_len >= CP_OEM_AUTH_SOCKET_PATH_MAX ||
        path[0] != '/' || path[path_len - 1u] == '/') {
        return auth_open_disabled(auth, CP_OEM_AUTH_ERR_ARGUMENT);
    }
    if (config->timeout_ms > CP_OEM_AUTH_RPC_MAX_TIMEOUT_MS) {
        return auth_open_disabled(auth, CP_OEM_AUTH_ERR_ARGUMENT);
    }
    err = auth_init_common(auth);
    if (err != CP_OEM_AUTH_OK) {
        return err;
    }
    memcpy(auth->socket_path, path, path_len + 1u);
    auth->timeout_ms = config->timeout_ms == 0u
                           ? CP_OEM_AUTH_RPC_DEFAULT_TIMEOUT_MS
                           : config->timeout_ms;
    auth->rpc = true;
    auth->experimental = true;
#ifdef CP_OEM_AUTH_TEST
    auth->expected_peer_uid = config->expected_peer_uid;
#endif
    return CP_OEM_AUTH_OK;
}

#ifdef CP_OEM_AUTH_TEST
cp_oem_auth_error cp_oem_auth_open_mock(cp_oem_auth *auth,
                                        const cp_oem_auth_native_ops *ops)
{
    cp_oem_auth_error err;

    if (auth == NULL || ops == NULL || ops->initialize == NULL ||
        ops->copy_certificate == NULL || ops->create_signature == NULL ||
        ops->release == NULL) {
        if (auth != NULL) {
            (void)auth_open_disabled(auth, CP_OEM_AUTH_ERR_ARGUMENT);
        }
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    err = auth_init_common(auth);
    if (err != CP_OEM_AUTH_OK) {
        return err;
    }
    auth->mock = true;
    auth->ops = *ops;
    auth->timeout_ms = CP_OEM_AUTH_RPC_DEFAULT_TIMEOUT_MS;
    return CP_OEM_AUTH_OK;
}
#endif

cp_boot_auth_provider cp_oem_auth_boot_provider(cp_oem_auth *auth)
{
    cp_boot_auth_provider provider;

    memset(&provider, 0, sizeof(provider));
    if (auth != NULL) {
        provider.context = auth;
        provider.certificate = cp_oem_auth_provider_certificate;
        provider.signature = cp_oem_auth_provider_signature;
    }
    return provider;
}

static cp_oem_auth_error auth_mock_ready_locked(cp_oem_auth *auth)
{
    if (!auth->backend_initialized) {
        cp_oem_auth_error err = cp_oem_auth_native_initialize(&auth->ops);

        auth->backend_initialized = true;
        auth->backend_error = err;
        if (err != CP_OEM_AUTH_OK) {
            return err;
        }
    }
    return auth->backend_error;
}

static cp_oem_auth_error auth_mock_certificate_locked(cp_oem_auth *auth,
                                                      uint8_t *out,
                                                      size_t cap,
                                                      size_t *out_len)
{
    cp_oem_auth_error err = auth_mock_ready_locked(auth);

    if (err != CP_OEM_AUTH_OK) {
        return err;
    }
    return cp_oem_auth_native_certificate(&auth->ops, out, cap, out_len);
}

static cp_oem_auth_error auth_mock_signature_locked(cp_oem_auth *auth,
                                                    const uint8_t *challenge,
                                                    size_t challenge_len,
                                                    uint8_t *out, size_t cap,
                                                    size_t *out_len)
{
    cp_oem_auth_error err = auth_mock_ready_locked(auth);

    if (err != CP_OEM_AUTH_OK) {
        return err;
    }
    return cp_oem_auth_native_signature(&auth->ops, challenge, challenge_len,
                                        out, cap, out_len);
}

int cp_oem_auth_provider_certificate(void *context, uint8_t *out, size_t cap,
                                     size_t *out_len)
{
    cp_oem_auth *auth = (cp_oem_auth *)context;
    cp_oem_auth_error err;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (auth == NULL || out == NULL || out_len == NULL ||
        !auth->initialized) {
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0) {
        return -1;
    }
    if (!auth->initialized) {
        err = CP_OEM_AUTH_ERR_INTERNAL;
    } else if (cap == 0u) {
        err = CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL;
    } else if (auth->mock) {
        err = auth_mock_certificate_locked(auth, out, cap, out_len);
    } else if (auth->rpc) {
        err = auth_rpc_exchange_locked(
            auth, CP_OEM_AUTH_RPC_OP_CERTIFICATE, NULL, 0u, out, cap,
            out_len);
    } else {
        err = CP_OEM_AUTH_ERR_INTERNAL;
    }
    auth->last_error = err;
    (void)pthread_mutex_unlock(&auth->lock);
    return err == CP_OEM_AUTH_OK ? 0 : -1;
}

int cp_oem_auth_provider_signature(void *context, const uint8_t *challenge,
                                   size_t challenge_len, uint8_t *out,
                                   size_t cap, size_t *out_len)
{
    cp_oem_auth *auth = (cp_oem_auth *)context;
    cp_oem_auth_error err;

    if (out_len != NULL) {
        *out_len = 0u;
    }
    if (auth == NULL || challenge == NULL || out == NULL || out_len == NULL ||
        !auth->initialized) {
        return -1;
    }
    if (pthread_mutex_lock(&auth->lock) != 0) {
        return -1;
    }
    if (!auth->initialized) {
        err = CP_OEM_AUTH_ERR_INTERNAL;
    } else if (challenge_len != CP_OEM_AUTH_CHALLENGE_SHA1 &&
               challenge_len != CP_OEM_AUTH_CHALLENGE_SHA256) {
        err = CP_OEM_AUTH_ERR_CHALLENGE_LENGTH;
    } else if (cap == 0u) {
        err = CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL;
    } else if (auth->mock) {
        err = auth_mock_signature_locked(auth, challenge, challenge_len, out,
                                         cap, out_len);
    } else if (auth->rpc) {
        err = auth_rpc_exchange_locked(auth, CP_OEM_AUTH_RPC_OP_SIGNATURE,
                                       challenge, challenge_len, out, cap,
                                       out_len);
    } else {
        err = CP_OEM_AUTH_ERR_INTERNAL;
    }
    auth->last_error = err;
    (void)pthread_mutex_unlock(&auth->lock);
    return err == CP_OEM_AUTH_OK ? 0 : -1;
}

cp_oem_auth_error cp_oem_auth_last_error(cp_oem_auth *auth)
{
    cp_oem_auth_error err;

    if (auth == NULL) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (!auth->initialized) {
        return auth->last_error;
    }
    if (pthread_mutex_lock(&auth->lock) != 0) {
        return CP_OEM_AUTH_ERR_INTERNAL;
    }
    err = auth->last_error;
    (void)pthread_mutex_unlock(&auth->lock);
    return err;
}

void cp_oem_auth_destroy(cp_oem_auth *auth)
{
    if (auth == NULL) {
        return;
    }
    if (auth->initialized) {
        bool locked = false;
        int fd = auth->fd;

        /* Close before attempting the lock so a lock fault cannot leak the
         * helper connection.  Concurrent destroy/use remains caller misuse. */
        auth->fd = -1;
        if (fd >= 0) {
            (void)close(fd);
        }
#ifdef CP_OEM_AUTH_TEST
        if (!auth->force_destroy_lock_failure) {
#endif
            locked = pthread_mutex_lock(&auth->lock) == 0;
#ifdef CP_OEM_AUTH_TEST
        }
#endif
        if (locked) {
            cp_oem_auth_secure_zero(auth->rx, sizeof(auth->rx));
            cp_oem_auth_secure_zero(auth->tx, sizeof(auth->tx));
            auth->initialized = false;
            (void)pthread_mutex_unlock(&auth->lock);
        }
        (void)pthread_mutex_destroy(&auth->lock);
    }
    cp_oem_auth_secure_zero(auth, sizeof(*auth));
}

const char *cp_oem_auth_error_name(cp_oem_auth_error error)
{
    switch (error) {
    case CP_OEM_AUTH_OK:
        return "ok";
    case CP_OEM_AUTH_ERR_ARGUMENT:
        return "argument";
    case CP_OEM_AUTH_ERR_DISABLED:
        return "disabled";
    case CP_OEM_AUTH_ERR_POLICY:
        return "policy";
    case CP_OEM_AUTH_ERR_LIBRARY:
        return "library";
    case CP_OEM_AUTH_ERR_HASH:
        return "hash";
    case CP_OEM_AUTH_ERR_SYMBOL:
        return "symbol";
    case CP_OEM_AUTH_ERR_INIT:
        return "init";
    case CP_OEM_AUTH_ERR_CERTIFICATE:
        return "certificate";
    case CP_OEM_AUTH_ERR_CHALLENGE_LENGTH:
        return "challenge-length";
    case CP_OEM_AUTH_ERR_SIGNATURE:
        return "signature";
    case CP_OEM_AUTH_ERR_OUTPUT_TOO_SMALL:
        return "output-too-small";
    case CP_OEM_AUTH_ERR_OOM:
        return "oom";
    case CP_OEM_AUTH_ERR_RPC:
        return "rpc";
    case CP_OEM_AUTH_ERR_RPC_TIMEOUT:
        return "rpc-timeout";
    case CP_OEM_AUTH_ERR_RPC_PEER:
        return "rpc-peer";
    case CP_OEM_AUTH_ERR_RPC_PROTOCOL:
        return "rpc-protocol";
    case CP_OEM_AUTH_ERR_INTERNAL:
        return "internal";
    default:
        return "unknown";
    }
}
