/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Tiny root helper for the hardware-backed MFi provider.
 *
 * Within this CarPlay integration, this helper is the only component that
 * loads libCoreUtils.so.  That is not a global claim about other processes or
 * integrations on the head unit.  It refuses to start without
 * --serve --experimental, verifies the pinned file SHA-256, resolves the three
 * MFiPlatform exports, creates a root-only SOCK_SEQPACKET socket (directory
 * 0700, socket 0600), checks SO_PEERCRED and serves bounded certificate and
 * signature requests.  It never logs certificate or signature bytes and never
 * touches /dev/i2c-1 itself; the OEM library does that on the target only.
 * The client RPC deadline bounds only the caller: it does not bound OEM/chip
 * work, and the helper may remain in provider code after the caller times out.
 * There is no hardware watchdog here.
 *
 * No OEM binary is executed on the build host.  Tests build this file with a
 * first-party mock .so and CP_OEM_AUTH_TEST.
 */

#define _GNU_SOURCE
#include "cp_oem_auth.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define CP_NATIVE_MAX_LIBRARY_BYTES (64u * 1024u * 1024u)

typedef int (*mfi_initialize_fn)(void);
typedef int (*mfi_certificate_fn)(void **out, size_t *out_len);
typedef int (*mfi_signature_fn)(const void *digest, size_t digest_len,
                                void **out, size_t *out_len);

static struct {
    void *handle;
    mfi_initialize_fn initialize;
    mfi_certificate_fn certificate;
    mfi_signature_fn signature;
    bool initialized;
    bool init_failed;
    cp_oem_auth_error init_error;
} g_mfi;

static int g_listen_fd = -1;
static char g_socket_path[CP_OEM_AUTH_SOCKET_PATH_MAX];
static char g_socket_dir[CP_OEM_AUTH_SOCKET_PATH_MAX];
static bool g_dir_created;
static bool g_socket_bound;
static dev_t g_socket_dev;
static ino_t g_socket_ino;
static uint8_t g_request[CP_OEM_AUTH_RPC_MAX_FRAME];
static uint8_t g_response[CP_OEM_AUTH_RPC_MAX_FRAME];
static uint8_t g_payload[CP_OEM_AUTH_MAX_CERTIFICATE];
static volatile sig_atomic_t g_stop;

/* ---------------------------------------------------------------- */
/* SHA-256 (first-party, no OpenSSL dependency in the helper)        */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t state[8];
    uint64_t length;
    uint8_t block[64];
    size_t used;
} sha256_ctx;

static const uint32_t g_sha256_k[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u
};

static uint32_t rotr32(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t sha256_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void sha256_block(sha256_ctx *ctx, const uint8_t *data)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t i;

    for (i = 0; i < 16u; i++) {
        w[i] = sha256_be32(data + i * 4u);
    }
    for (i = 16u; i < 64u; i++) {
        uint32_t s0 = rotr32(w[i - 15u], 7u) ^
                      rotr32(w[i - 15u], 18u) ^ (w[i - 15u] >> 3);
        uint32_t s1 = rotr32(w[i - 2u], 17u) ^
                      rotr32(w[i - 2u], 19u) ^ (w[i - 2u] >> 10);
        w[i] = w[i - 16u] + s0 + w[i - 7u] + s1;
    }
    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];
    for (i = 0; i < 64u; i++) {
        uint32_t s1 = rotr32(e, 6u) ^ rotr32(e, 11u) ^ rotr32(e, 25u);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + s1 + ch + g_sha256_k[i] + w[i];
        uint32_t s0 = rotr32(a, 2u) ^ rotr32(a, 13u) ^ rotr32(a, 22u);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx)
{
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->length = 0u;
    ctx->used = 0u;
}

static void sha256_update(sha256_ctx *ctx, const uint8_t *data, size_t len)
{
    ctx->length += (uint64_t)len;
    while (len != 0u) {
        size_t room = sizeof(ctx->block) - ctx->used;
        size_t take = len < room ? len : room;

        memcpy(ctx->block + ctx->used, data, take);
        ctx->used += take;
        data += take;
        len -= take;
        if (ctx->used == sizeof(ctx->block)) {
            sha256_block(ctx, ctx->block);
            ctx->used = 0u;
        }
    }
}

static void sha256_final(sha256_ctx *ctx, uint8_t out[32])
{
    uint64_t bits = ctx->length * 8u;
    size_t i = ctx->used;

    ctx->block[i++] = 0x80u;
    if (i > 56u) {
        while (i < 64u) {
            ctx->block[i++] = 0u;
        }
        sha256_block(ctx, ctx->block);
        i = 0u;
    }
    while (i < 56u) {
        ctx->block[i++] = 0u;
    }
    for (i = 0; i < 8u; i++) {
        ctx->block[56u + i] = (uint8_t)(bits >> (56u - 8u * i));
    }
    sha256_block(ctx, ctx->block);
    for (i = 0; i < 8u; i++) {
        out[4u * i] = (uint8_t)(ctx->state[i] >> 24);
        out[4u * i + 1u] = (uint8_t)(ctx->state[i] >> 16);
        out[4u * i + 2u] = (uint8_t)(ctx->state[i] >> 8);
        out[4u * i + 3u] = (uint8_t)ctx->state[i];
    }
    cp_oem_auth_secure_zero(ctx, sizeof(*ctx));
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool parse_sha256(const char *text, uint8_t out[32])
{
    size_t i;

    if (text == NULL || strlen(text) != 64u) {
        return false;
    }
    for (i = 0; i < 32u; i++) {
        int hi = hex_value(text[2u * i]);
        int lo = hex_value(text[2u * i + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static int auth_fstat(int fd, struct stat *st)
{
    return (int)syscall(SYS_fstat, fd, st);
}

static int auth_lstat(const char *path, struct stat *st)
{
    return (int)syscall(SYS_newfstatat, AT_FDCWD, path, st,
                        AT_SYMLINK_NOFOLLOW);
}

static bool hash_fd(int fd, uint8_t out[32])
{
    sha256_ctx ctx;
    uint8_t buffer[8192];
    off_t size;

    size = lseek(fd, 0, SEEK_END);
    if (size <= 0 || (uint64_t)size > CP_NATIVE_MAX_LIBRARY_BYTES) {
        return false;
    }
    if (lseek(fd, 0, SEEK_SET) != 0) {
        return false;
    }
    sha256_init(&ctx);
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            cp_oem_auth_secure_zero(&ctx, sizeof(ctx));
            return false;
        }
        if (got == 0) {
            break;
        }
        sha256_update(&ctx, buffer, (size_t)got);
    }
    cp_oem_auth_secure_zero(buffer, sizeof(buffer));
    sha256_final(&ctx, out);
    return true;
}

/* ---------------------------------------------------------------- */
/* Native binding                                                     */
/* ---------------------------------------------------------------- */

static int native_initialize(void *context)
{
    (void)context;
    return g_mfi.initialize();
}

static int native_certificate(void *context, void **out, size_t *out_len)
{
    (void)context;
    return g_mfi.certificate(out, out_len);
}

static int native_signature(void *context, const void *digest,
                            size_t digest_len, void **out, size_t *out_len)
{
    (void)context;
    return g_mfi.signature(digest, digest_len, out, out_len);
}

static void native_release(void *context, void *buffer, size_t len)
{
    (void)context;
    (void)len;
    free(buffer);
}

static const cp_oem_auth_native_ops g_native_ops = {
    NULL,
    native_initialize,
    native_certificate,
    native_signature,
    native_release
};

static bool make_fd_path(int fd, char *out, size_t cap)
{
    static const char prefix[] = "/proc/self/fd/";
    char digits[16];
    size_t count = 0u;
    size_t prefix_len = sizeof(prefix) - 1u;
    unsigned int value = (unsigned int)fd;

    if (fd < 0 || out == NULL || cap == 0u) {
        return false;
    }
    do {
        digits[count++] = (char)('0' + (value % 10u));
        value /= 10u;
    } while (value != 0u && count < sizeof(digits));
    if (value != 0u || prefix_len + count + 1u > cap) {
        return false;
    }
    memcpy(out, prefix, prefix_len);
    for (size_t i = 0; i < count; i++) {
        out[prefix_len + i] = digits[count - 1u - i];
    }
    out[prefix_len + count] = '\0';
    return true;
}

static bool load_symbol(void *handle, const char *name, void *destination,
                        size_t destination_size)
{
    void *symbol;
    const char *error;

    if (destination == NULL || destination_size != sizeof(void *)) {
        return false;
    }
    (void)dlerror();
    symbol = dlsym(handle, name);
    error = dlerror();
    if (symbol == NULL || error != NULL) {
        return false;
    }
    memcpy(destination, &symbol, destination_size);
    return true;
}

static bool allowed_loader_dir(const char *component, size_t length)
{
    static const char *const allowed[] = {
        "/app/lib", "/lib", "/usr/lib", "/lib64", "/usr/lib64"
    };
    size_t i;

    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (length == strlen(allowed[i]) &&
            memcmp(component, allowed[i], length) == 0) {
            return true;
        }
    }
    return false;
}

/* Reject inherited loader redirection instead of silently trusting it.  A
 * service that needs LD_LIBRARY_PATH may only name explicit target dirs;
 * anything else is a fail-closed activation error. */
static bool sanitize_loader_env(void)
{
    static const char *const forbidden[] = {
        "LD_PRELOAD", "LD_AUDIT", "LD_DEBUG", "LD_PROFILE",
        "LD_USE_LOAD_BIAS", "LD_ORIGIN_PATH", "LD_ASSUME_KERNEL",
        "LD_TRACE_LOADED_OBJECTS", "LD_WARN", "LD_VERBOSE", "LD_BIND_NOT",
        "LD_PREFER_MAP_32BIT_EXEC", "LD_HWCAP_MASK", "LD_SHOW_AUXV",
        "LD_DYNAMIC_WEAK"
    };
    const char *path;
    size_t i;

    for (i = 0; i < sizeof(forbidden) / sizeof(forbidden[0]); i++) {
        const char *value = getenv(forbidden[i]);

        if (value != NULL && value[0] != '\0') {
            return false;
        }
    }
    path = getenv("LD_LIBRARY_PATH");
    if (path == NULL || path[0] == '\0') {
        return true;
    }
    for (;;) {
        const char *colon = strchr(path, ':');
        size_t length = colon != NULL ? (size_t)(colon - path) : strlen(path);

        if (length == 0u || path[0] != '/' ||
            !allowed_loader_dir(path, length)) {
            return false;
        }
        if (colon == NULL) {
            return true;
        }
        path = colon + 1;
        if (path[0] == '\0') {
            return false; /* trailing colon means an unsafe cwd search */
        }
    }
}

static cp_oem_auth_error load_library(const char *path,
                                      const char *expected_sha)
{
    uint8_t expected[32];
    uint8_t actual[32];
    char proc_path[64];
    struct stat stat_fd;
    int fd;
    void *handle;

    if (path == NULL || expected_sha == NULL ||
        !parse_sha256(expected_sha, expected)) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return CP_OEM_AUTH_ERR_LIBRARY;
    }
    if (auth_fstat(fd, &stat_fd) != 0 || !S_ISREG(stat_fd.st_mode) ||
        stat_fd.st_size <= 0 ||
        (uint64_t)stat_fd.st_size > CP_NATIVE_MAX_LIBRARY_BYTES) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_LIBRARY;
    }
    if (!hash_fd(fd, actual) || memcmp(expected, actual, sizeof(actual)) != 0) {
        cp_oem_auth_secure_zero(actual, sizeof(actual));
        (void)close(fd);
        return CP_OEM_AUTH_ERR_HASH;
    }
    cp_oem_auth_secure_zero(actual, sizeof(actual));

    if (!make_fd_path(fd, proc_path, sizeof(proc_path))) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_LIBRARY;
    }
    /* Load exactly the hashed inode.  Never fall back to an unverified path:
     * /proc/self/fd is the only activation source; failure is fail-closed. */
    handle = dlopen(proc_path, RTLD_NOW | RTLD_LOCAL);
    (void)close(fd);
    if (handle == NULL) {
        return CP_OEM_AUTH_ERR_LIBRARY;
    }
    if (!load_symbol(handle, CP_OEM_AUTH_MFI_INITIALIZE_SYMBOL,
                     &g_mfi.initialize, sizeof(g_mfi.initialize)) ||
        !load_symbol(handle, CP_OEM_AUTH_MFI_CERTIFICATE_SYMBOL,
                     &g_mfi.certificate, sizeof(g_mfi.certificate)) ||
        !load_symbol(handle, CP_OEM_AUTH_MFI_SIGNATURE_SYMBOL,
                     &g_mfi.signature, sizeof(g_mfi.signature))) {
        (void)dlclose(handle);
        memset(&g_mfi, 0, sizeof(g_mfi));
        return CP_OEM_AUTH_ERR_SYMBOL;
    }
    g_mfi.handle = handle;
    return CP_OEM_AUTH_OK;
}

/* ---------------------------------------------------------------- */
/* Socket lifecycle                                                   */
/* ---------------------------------------------------------------- */

static bool parent_dir(const char *path, char *out, size_t cap)
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
        n = 1u;
    }
    if (n + 1u > cap) {
        return false;
    }
    memcpy(out, path, n);
    out[n] = '\0';
    return true;
}

static cp_oem_auth_error prepare_socket_path(const char *path,
                                             uint32_t expected_uid)
{
    struct stat st;
    cp_oem_auth_error error = CP_OEM_AUTH_OK;

    if (path == NULL || path[0] != '/' ||
        strlen(path) >= CP_OEM_AUTH_SOCKET_PATH_MAX ||
        path[strlen(path) - 1u] == '/') {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    memcpy(g_socket_path, path, strlen(path) + 1u);
    if (!parent_dir(g_socket_path, g_socket_dir, sizeof(g_socket_dir))) {
        return CP_OEM_AUTH_ERR_ARGUMENT;
    }
    if (auth_lstat(g_socket_dir, &st) != 0) {
        if (errno != ENOENT) {
            return CP_OEM_AUTH_ERR_POLICY;
        }
        if (mkdir(g_socket_dir, 0700) != 0) {
            return CP_OEM_AUTH_ERR_POLICY;
        }
        g_dir_created = true;
        if (chmod(g_socket_dir, 0700) != 0) {
            return CP_OEM_AUTH_ERR_POLICY;
        }
        if (auth_lstat(g_socket_dir, &st) != 0) {
            return CP_OEM_AUTH_ERR_POLICY;
        }
    }
    if (!S_ISDIR(st.st_mode) || (uint32_t)st.st_uid != expected_uid ||
        (st.st_mode & 0777u) != 0700u) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
    if (auth_lstat(g_socket_path, &st) == 0) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
    if (errno != ENOENT) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
    return error;
}

static cp_oem_auth_error open_socket(uint32_t expected_uid)
{
    struct sockaddr_un addr;
    struct stat path_stat;
    int fd;

    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return CP_OEM_AUTH_ERR_RPC;
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, g_socket_path, strlen(g_socket_path) + 1u);
    if (bind(fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_RPC;
    }
    g_socket_bound = true;
    /* The path inode is the ownership token for cleanup.  fstat() on the
     * socket fd reports the sockfs inode, not the filesystem dentry. */
    if (auth_lstat(g_socket_path, &path_stat) != 0 ||
        !S_ISSOCK(path_stat.st_mode) ||
        (uint32_t)path_stat.st_uid != expected_uid) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_POLICY;
    }
    g_socket_dev = path_stat.st_dev;
    g_socket_ino = path_stat.st_ino;
    if (chmod(g_socket_path, 0600) != 0) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_POLICY;
    }
    if (listen(fd, 8) != 0) {
        (void)close(fd);
        return CP_OEM_AUTH_ERR_RPC;
    }
    g_listen_fd = fd;
    return CP_OEM_AUTH_OK;
}

static void cleanup(void)
{
    if (g_listen_fd >= 0) {
        (void)close(g_listen_fd);
        g_listen_fd = -1;
    }
    if (g_socket_bound) {
        struct stat st;
        if (auth_lstat(g_socket_path, &st) == 0 && S_ISSOCK(st.st_mode) &&
            st.st_dev == g_socket_dev && st.st_ino == g_socket_ino) {
            (void)unlink(g_socket_path);
        }
        g_socket_bound = false;
    }
    if (g_dir_created) {
        (void)rmdir(g_socket_dir);
        g_dir_created = false;
    }
    if (g_mfi.handle != NULL) {
        (void)dlclose(g_mfi.handle);
        g_mfi.handle = NULL;
    }
    memset(&g_mfi, 0, sizeof(g_mfi));
    cp_oem_auth_secure_zero(g_request, sizeof(g_request));
    cp_oem_auth_secure_zero(g_response, sizeof(g_response));
    cp_oem_auth_secure_zero(g_payload, sizeof(g_payload));
    cp_oem_auth_secure_zero(g_socket_path, sizeof(g_socket_path));
    cp_oem_auth_secure_zero(g_socket_dir, sizeof(g_socket_dir));
}

static bool set_socket_timeout(int fd, uint32_t timeout_ms)
{
    struct timeval tv;

    tv.tv_sec = (time_t)(timeout_ms / 1000u);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000u) * 1000u);
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

static bool peer_is_expected(int fd, uint32_t expected_uid)
{
    struct ucred cred;
    socklen_t cred_len = (socklen_t)sizeof(cred);

    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) != 0 ||
        cred_len != (socklen_t)sizeof(cred)) {
        return false;
    }
    return (uint32_t)cred.uid == expected_uid;
}

/* ---------------------------------------------------------------- */
/* Request processing                                                 */
/* ---------------------------------------------------------------- */

static cp_oem_auth_error ensure_initialized(void)
{
    cp_oem_auth_error error;

    if (g_mfi.initialized) {
        return CP_OEM_AUTH_OK;
    }
    if (g_mfi.init_failed) {
        return g_mfi.init_error;
    }
    error = cp_oem_auth_native_initialize(&g_native_ops);
    if (error != CP_OEM_AUTH_OK) {
        /* Latch the permanent backend initialization failure.  Never call
         * certificate or signature after a failed OEM initialize. */
        g_mfi.init_failed = true;
        g_mfi.init_error = error;
        return error;
    }
    g_mfi.initialized = true;
    return CP_OEM_AUTH_OK;
}

static bool write_all(int fd, const uint8_t *data, size_t len)
{
    size_t sent = 0u;

    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

static bool read_record(int fd, uint32_t timeout_ms, size_t *length)
{
    struct pollfd pfd;
    ssize_t got;

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    for (;;) {
        int rc = poll(&pfd, 1, (int)timeout_ms);
        if (rc == 0) {
            return false;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        break;
    }
    got = recv(fd, g_request, sizeof(g_request), MSG_TRUNC);
    if (got <= 0) {
        return false;
    }
    *length = (size_t)got;
    return true;
}

static bool send_response(int fd, uint16_t op, uint32_t request_id,
                          cp_oem_auth_error status, const uint8_t *payload,
                          size_t payload_len)
{
    cp_oem_auth_rpc_response response;
    size_t total;

    if (payload_len > sizeof(g_response) - CP_OEM_AUTH_RPC_RESPONSE_LEN) {
        return false;
    }
    memset(&response, 0, sizeof(response));
    response.magic = CP_OEM_AUTH_RPC_MAGIC;
    response.version = CP_OEM_AUTH_RPC_VERSION;
    response.op = op;
    response.request_id = request_id;
    response.status = (int32_t)status;
    response.payload_len = (uint32_t)payload_len;
    memset(g_response, 0, sizeof(g_response));
    cp_oem_auth_rpc_response_encode(g_response, &response);
    if (payload_len != 0u) {
        memcpy(g_response + CP_OEM_AUTH_RPC_RESPONSE_LEN, payload,
               payload_len);
    }
    total = CP_OEM_AUTH_RPC_RESPONSE_LEN + payload_len;
    if (total > sizeof(g_response) || !write_all(fd, g_response, total)) {
        cp_oem_auth_secure_zero(g_response, sizeof(g_response));
        return false;
    }
    cp_oem_auth_secure_zero(g_response, sizeof(g_response));
    return true;
}

static bool handle_connection(int fd, uint32_t timeout_ms)
{
    for (;;) {
        cp_oem_auth_rpc_request request;
        cp_oem_auth_error status;
        size_t request_len;
        size_t payload_len = 0u;

        if (!read_record(fd, timeout_ms, &request_len)) {
            return true;
        }
        if (request_len < CP_OEM_AUTH_RPC_REQUEST_LEN ||
            request_len > sizeof(g_request) ||
            !cp_oem_auth_rpc_request_decode(g_request,
                                            CP_OEM_AUTH_RPC_REQUEST_LEN,
                                            &request) ||
            request.payload_len != request_len -
                                      CP_OEM_AUTH_RPC_REQUEST_LEN) {
            return false;
        }
        status = ensure_initialized();
        if (status != CP_OEM_AUTH_OK) {
            if (!send_response(fd, request.op, request.request_id, status,
                               NULL, 0u)) {
                return false;
            }
            continue;
        }
        if (request.op == CP_OEM_AUTH_RPC_OP_CERTIFICATE) {
            status = cp_oem_auth_native_certificate(
                &g_native_ops, g_payload, sizeof(g_payload), &payload_len);
        } else {
            status = cp_oem_auth_native_signature(
                &g_native_ops,
                g_request + CP_OEM_AUTH_RPC_REQUEST_LEN,
                request.payload_len, g_payload, sizeof(g_payload),
                &payload_len);
        }
        if (!send_response(fd, request.op, request.request_id, status,
                           status == CP_OEM_AUTH_OK ? g_payload : NULL,
                           status == CP_OEM_AUTH_OK ? payload_len : 0u)) {
            cp_oem_auth_secure_zero(g_request, sizeof(g_request));
            cp_oem_auth_secure_zero(g_payload, sizeof(g_payload));
            return false;
        }
        cp_oem_auth_secure_zero(g_request, sizeof(g_request));
        cp_oem_auth_secure_zero(g_payload, sizeof(g_payload));
    }
}

/* ---------------------------------------------------------------- */
/* CLI and serve loop                                                 */
/* ---------------------------------------------------------------- */

typedef struct {
    bool serve;
    bool experimental;
    bool once;
    const char *library;
    const char *socket;
    const char *expected_sha;
    uint32_t timeout_ms;
} options;

static bool parse_u32(const char *text, uint32_t *value)
{
    uint64_t parsed = 0u;
    size_t i = 0u;

    if (text == NULL || value == NULL || text[0] == '\0') {
        return false;
    }
    while (text[i] != '\0') {
        if (text[i] < '0' || text[i] > '9') {
            return false;
        }
        parsed = parsed * 10u + (uint64_t)(text[i] - '0');
        if (parsed > UINT32_MAX) {
            return false;
        }
        i++;
    }
    *value = (uint32_t)parsed;
    return true;
}

static int parse_options(int argc, char **argv, options *opts)
{
    int i;

    memset(opts, 0, sizeof(*opts));
    opts->library = CP_OEM_AUTH_COREUTILS_DEFAULT_PATH;
    opts->socket = CP_OEM_AUTH_RPC_DEFAULT_SOCKET;
    opts->expected_sha = CP_OEM_AUTH_COREUTILS_SHA256;
    opts->timeout_ms = CP_OEM_AUTH_RPC_DEFAULT_TIMEOUT_MS;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--serve") == 0) {
            opts->serve = true;
        } else if (strcmp(argv[i], "--experimental") == 0) {
            opts->experimental = true;
#ifdef CP_OEM_AUTH_TEST
        } else if (strcmp(argv[i], "--once") == 0) {
            opts->once = true;
        } else if (strcmp(argv[i], "--expected-sha256") == 0) {
            if (++i >= argc) {
                return CP_OEM_AUTH_ERR_ARGUMENT;
            }
            opts->expected_sha = argv[i];
#endif
        } else if (strcmp(argv[i], "--library") == 0) {
            if (++i >= argc) {
                return CP_OEM_AUTH_ERR_ARGUMENT;
            }
            opts->library = argv[i];
        } else if (strcmp(argv[i], "--socket") == 0) {
            if (++i >= argc) {
                return CP_OEM_AUTH_ERR_ARGUMENT;
            }
            opts->socket = argv[i];
        } else if (strcmp(argv[i], "--timeout-ms") == 0) {
            if (++i >= argc || !parse_u32(argv[i], &opts->timeout_ms) ||
                opts->timeout_ms == 0u ||
                opts->timeout_ms > CP_OEM_AUTH_RPC_MAX_TIMEOUT_MS) {
                return CP_OEM_AUTH_ERR_ARGUMENT;
            }
        } else {
            return CP_OEM_AUTH_ERR_ARGUMENT;
        }
    }
    return CP_OEM_AUTH_OK;
}

static void on_signal(int signal_number)
{
    (void)signal_number;
    g_stop = 1;
}

static void install_signals(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    (void)sigaction(SIGINT, &action, NULL);
    (void)sigaction(SIGTERM, &action, NULL);
}

#ifdef CP_OEM_AUTH_TEST
static uint32_t expected_uid(void)
{
    return (uint32_t)geteuid();
}
#else
static uint32_t expected_uid(void)
{
    return 0u;
}
#endif

int main(int argc, char **argv)
{
    options opts;
    cp_oem_auth_error error;
    uint32_t uid;
    int rc;

    rc = parse_options(argc, argv, &opts);
    if (rc != CP_OEM_AUTH_OK) {
        return rc;
    }
    if (!opts.serve) {
        return CP_OEM_AUTH_ERR_DISABLED;
    }
    if (!opts.experimental) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
    uid = expected_uid();
#ifndef CP_OEM_AUTH_TEST
    if (geteuid() != 0u || uid != 0u) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
#endif
    /* Sanitize the process environment, working directory and umask before
     * dlopen runs any OEM constructor.  Path validation happens first too, so
     * a bad activation path cannot reach OEM code. */
    if (!sanitize_loader_env() || chdir("/") != 0) {
        return CP_OEM_AUTH_ERR_POLICY;
    }
    (void)umask(0077);
    error = prepare_socket_path(opts.socket, uid);
    if (error != CP_OEM_AUTH_OK) {
        cleanup();
        return (int)error;
    }
    error = load_library(opts.library, opts.expected_sha);
    if (error != CP_OEM_AUTH_OK) {
        cleanup();
        return (int)error;
    }
    error = open_socket(uid);
    if (error != CP_OEM_AUTH_OK) {
        cleanup();
        return (int)error;
    }
    install_signals();
    while (!g_stop) {
        int client = accept(g_listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) {
                continue;
            }
            cleanup();
            return CP_OEM_AUTH_ERR_RPC;
        }
        (void)fcntl(client, F_SETFD, FD_CLOEXEC);
        if (peer_is_expected(client, uid) &&
            set_socket_timeout(client, opts.timeout_ms)) {
            (void)handle_connection(client, opts.timeout_ms);
        }
        (void)close(client);
        if (opts.once) {
            break;
        }
    }
    cleanup();
    return 0;
}
