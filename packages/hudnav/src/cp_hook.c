/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CarPlay iAP2 production interposer — LD_PRELOAD for iAP2Service-2.0.
 *
 * Thin hook layer around the host-tested pure logic in cp_shim.c. Everything
 * here is FAIL-OPEN: the original read/write return value and buffer are
 * preserved, and no error path drops or delays a vendor frame.
 *
 * Wire facts (docs/hudnav/CP_PROTOCOL.md, from the real 260319 binary):
 *   read seam:  read(mReaderFd @ +0x84, buf, 0x101d0) at 0xddd9
 *   write seam: write(mWriterFD @ +0x88, body, len) at 0xe7a1
 *   body:       [0x02][session-byte][40 40 <len BE16> <msgid BE16> <TLVs>]
 *   registration lives in Identify TLV 6 (sent) / 7 (recv), 2-byte BE ids
 *   0x5200 is NOT built by the stock binary; we submit it through the fd.
 *
 * Activation: add `Environment=LD_PRELOAD=.../cp_hook.so` to the data-only
 * systemd unit /lib/systemd/system/carplay_iAP2.service (reversible, no ELF
 * patch). The shim self-gates on /proc/self/exe == iAP2Service-2.0.
 *
 * Host-test overrides (never set on the car):
 *   CP_HOOK_FORCE=1  bypass the exe-name gate
 *   CP_HOOK_FD=<n>   treat fd n as the iAP2 link even if open() was not hooked
 *
 * glibc compatibility: no dlsym at all (raw syscalls for read/write/open). This library is also
 * listed in /etc/ld.so.preload, i.e. loaded into every process, and any dlsym reference makes
 * ld.so abort processes that do not load libdl on the target's glibc 2.25 (see below).
 */
#define _GNU_SOURCE
#include "cp_shim.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

/* NO dlsym here, deliberately. This library is listed in /etc/ld.so.preload, so it is loaded into
 * EVERY process. On the target's glibc 2.25 dlsym lives in libdl.so.2, which most processes do not
 * load; even a WEAK, address-taken reference to dlsym@GLIBC_2.2.5 makes ld.so abort the process at
 * startup ("symbol dlsym, version GLIBC_2.2.5 not defined in file libc.so.6") -- VM rehearsal
 * 2026-11-25, every command including rm failed. The real read/write/open are reached with raw
 * syscalls instead (identical semantics for these three calls). */

/* Avoid atoi(): on modern glibc it resolves to __isoc23_strtol@GLIBC_2.38,
 * which does not exist on the target's 2.25. */
static int atoi_small(const char *s)
{
    int v = 0;
    if (!s) return -1;
    for (; *s >= '0' && *s <= '9'; s++) {
        if (v > 1000000) return -1;
        v = v * 10 + (*s - '0');
    }
    return v;
}

typedef ssize_t (*read_fn)(int, void *, size_t);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef int (*open_fn)(const char *, int, ...);

static read_fn real_read;
static write_fn real_write;
static open_fn real_open;

/* fds of /dev/ttyIAP20. iAP2Service holds TWO (reader and writer); remembering only the last one
 * made the Identify write (on the other fd) invisible: first live CarPlay test, 2026-11-25. */
#define MAX_LINK_FDS 8
static int      link_fds[MAX_LINK_FDS] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static int      link_next;
static uint8_t  link_session;      /* copied from an observed outbound frame */
static int      have_session;
static int      start_sent;        /* one 0x5200 per link */
static __thread int in_hook;

static int gate_ok(void)
{
    static int g = -1;
    if (g >= 0) return g;
    const char *force = getenv("CP_HOOK_FORCE");
    if (force && force[0] == '1' && force[1] == '\0') { g = 1; return g; }
    char exe[256];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = '\0';
        const char *base = strrchr(exe, '/');
        base = base ? base + 1 : exe;
        g = (strcmp(base, "iAP2Service-2.0") == 0);
    } else {
        g = 0;
    }
    return g;
}

static int is_link_fd(int fd)
{
    const char *override = getenv("CP_HOOK_FD");
    if (override && atoi_small(override) == fd) return 1;
    if (fd < 0) return 0;
    for (int i = 0; i < MAX_LINK_FDS; i++)
        if (link_fds[i] == fd) return 1;
    return 0;
}

static void init_syms(void)
{
    /* real_read/real_write/real_open stay NULL: every call goes through the raw-syscall branches. */
}

/* A bounded diagnostic line; off by default. Never reads target content as
 * instructions, only logs fd/count/size numbers. */
static void trace(const char *tag, int fd, size_t n)
{
    const char *path = getenv("CP_HOOK_LOG");
    if (!path) return;
    int flags = O_WRONLY | O_CREAT | O_APPEND;
    int fdlog = real_open ? real_open(path, flags, 0644) : (int)syscall(SYS_openat, -100, path, flags, 0644);
    if (fdlog < 0) return;
    char line[128];
    int len = snprintf(line, sizeof line, "%s fd=%d n=%zu\n", tag, fd, n);
    if (len > 0) {
        ssize_t ignored = real_write ? real_write(fdlog, line, (size_t)len)
                                     : syscall(SYS_write, fdlog, line, (size_t)len);
        (void)ignored;
    }
    syscall(SYS_close, fdlog);   /* close is not interposed; raw syscall is safe */
}

/* Bounded diagnostic (CP_HOOK_LOG only): the first 10 bytes of the first 80 writes on the iAP2 link
 * fds. Protocol framing bytes only; used to see what the vendor really writes. */
static void trace_head(int fd, const uint8_t *b, size_t count)
{
    static int logged;
    const char *path = getenv("CP_HOOK_LOG");
    if (!path || logged >= 80) return;
    logged++;
    int fdlog = (int)syscall(SYS_openat, -100, path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fdlog < 0) return;
    char line[160];
    size_t k = count < 10 ? count : 10;
    int len = snprintf(line, sizeof line, "W_HEAD fd=%d count=%zu", fd, count);
    for (size_t i = 0; i < k && len > 0 && (size_t)len < sizeof line - 4; i++)
        len += snprintf(line + len, sizeof line - (size_t)len, " %02x", b[i]);
    if (len > 0 && (size_t)len < sizeof line - 2) { line[len++] = '\n'; }
    if (len > 0) { ssize_t ignored = syscall(SYS_write, fdlog, line, (size_t)len); (void)ignored; }
    syscall(SYS_close, fdlog);
}

/* Optional raw Identify dump for the first car session (CP_HOOK_DUMP=path).
 * Bounded to 4 frames, raw link-body bytes, so we can pin the param-30 value
 * and the registration lists from the real phone instead of guessing. */
static void dump_identify(const uint8_t *b, size_t n)
{
    static int dumps;
    if (dumps >= 4) return;
    const char *path = getenv("CP_HOOK_DUMP");
    if (!path) return;
    int fd = real_open ? real_open(path, O_WRONLY | O_CREAT | O_APPEND, 0600)
                       : (int)syscall(SYS_openat, -100, path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    uint8_t hdr[4] = { 'I', 'D', (uint8_t)(n >> 8), (uint8_t)n };
    ssize_t ignored = real_write ? real_write(fd, hdr, sizeof hdr)
                                 : syscall(SYS_write, fd, hdr, sizeof hdr);
    (void)ignored;
    ignored = real_write ? real_write(fd, b, n) : syscall(SYS_write, fd, b, n);
    (void)ignored;
    syscall(SYS_close, fd);
    dumps++;
}

/* ---- open: remember the iAP2 TTY fd -------------------------------------- */

int open(const char *path, int flags, ...)
{
    init_syms();
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, mode_t); va_end(ap);
    }
    int fd = real_open ? real_open(path, flags, mode)
                       : (int)syscall(SYS_openat, -100, path, flags, mode);
    if (fd >= 0 && strstr(path, "ttyIAP20")) {
        int known = 0;
        for (int i = 0; i < MAX_LINK_FDS; i++) if (link_fds[i] == fd) known = 1;
        if (!known) { link_fds[link_next] = fd; link_next = (link_next + 1) % MAX_LINK_FDS; }
        trace("TTY_OPEN", fd, (size_t)flags);
    }
    return fd;
}

/* ---- write: patch Identify, then submit 0x5200 once ---------------------- */

ssize_t write(int fd, const void *buf, size_t count)
{
    if (in_hook || !gate_ok() || !buf || count < 6 || count > 0x10000) {
        init_syms();
        return real_write ? real_write(fd, buf, count)
                          : (ssize_t)syscall(SYS_write, fd, buf, count);
    }
    init_syms();

    const uint8_t *b = (const uint8_t *)buf;
    if (is_link_fd(fd)) trace_head(fd, b, count);
    size_t base = (b[0] == 0x02 && count >= 8 && b[2] == 0x40 && b[3] == 0x40) ? 2 : 0;
    bool is_identify = false;
    if (base + 6 <= count && b[base] == 0x40 && b[base + 1] == 0x40) {
        uint16_t mlen = (uint16_t)((b[base + 2] << 8) | b[base + 3]);
        uint16_t msgid = (uint16_t)((b[base + 4] << 8) | b[base + 5]);
        is_identify = (msgid == CP_MSG_IDENTIFY && mlen >= 6 && base + mlen == count);
    }

    const void *out = buf;
    size_t outn = count;
    uint8_t *copy = NULL;

    {   /* bounded diagnostics: message ids seen on writes (numbers only, no payload) */
        static int logged_w;
        if (base + 6 <= count && b[base] == 0x40 && b[base + 1] == 0x40 && logged_w < 120) {
            logged_w++;
            trace(is_link_fd(fd) ? "W_MSG_LINK" : "W_MSG_OTHERFD", fd,
                  (size_t)((b[base + 4] << 8) | b[base + 5]));
        }
    }

    if (is_identify && is_link_fd(fd)) {
        copy = malloc(count + 64);
        if (copy) {
            memcpy(copy, buf, count);
            size_t n = cp_patch_identify(copy, count + 64, count);
            if (n > count && n != count) {     /* patched successfully */
                out = copy; outn = n;
                if (!have_session && base == 2) {
                    link_session = b[1]; have_session = 1;
                }
                dump_identify(out, outn);
                trace("CP_IDENTIFY_PATCHED", fd, n);
            }
        }
    } else if (!have_session && is_link_fd(fd) && base == 2) {
        link_session = b[1]; have_session = 1; /* learn session from any frame */
    }

    in_hook = 1;
    ssize_t ret = real_write ? real_write(fd, out, outn)
                             : (ssize_t)syscall(SYS_write, fd, out, outn);
    in_hook = 0;
    free(copy);

    /* Submit StartRouteGuidanceUpdates after a successful patched Identify.
     * This is the one place we generate a frame; it goes through the same fd. */
    if (is_identify && is_link_fd(fd) && have_session && !start_sent &&
        ret > 0 && outn != count) {
        uint8_t start[8];
        size_t sn = cp_build_start_link(start, sizeof start, link_session);
        if (sn == sizeof start) {
            start_sent = 1;
            in_hook = 1;
            ssize_t ignored = real_write ? real_write(fd, start, sn)
                                         : (ssize_t)syscall(SYS_write, fd, start, sn);
            in_hook = 0;
            (void)ignored;
            trace("CP_START_SENT", fd, sn);
        }
    }
    /* Return the caller's original count on success: the vendor must see the
     * write as a normal complete write even though we patched on the wire. */
    return ret > 0 ? (ssize_t)count : ret;
}

/* ---- read: observe (never modify) inbound route-guidance messages -------- */

ssize_t read(int fd, void *buf, size_t count)
{
    init_syms();
    ssize_t ret = real_read ? real_read(fd, buf, count)
                            : (ssize_t)syscall(SYS_read, fd, buf, count);
    if (in_hook || !gate_ok() || ret <= 0 || !buf ||
        (!is_link_fd(fd) && count != 0x101d0u))
        return ret;                              /* original value, unchanged */

    const uint8_t *p = (const uint8_t *)buf;
    size_t left = (size_t)ret;
    while (left >= 6) {
        size_t off = 0;
        while (off + 6 <= left && !(p[off] == 0x40 && p[off + 1] == 0x40)) off++;
        if (off + 6 > left) break;
        uint16_t mlen = (uint16_t)((p[off + 2] << 8) | p[off + 3]);
        if (mlen < 6 || mlen > left - off) break;
        {   /* bounded diagnostics: message ids seen on reads */
            static int logged_r;
            if (logged_r < 120) { logged_r++; trace("R_MSG", fd, (size_t)((p[off + 4] << 8) | p[off + 5])); }
        }
        cp_handle_message(p + off, mlen, cp_sock_emit, NULL);
        left -= off + mlen;
        p += off + mlen;
    }
    return ret;                                  /* original buffer, untouched */
}
