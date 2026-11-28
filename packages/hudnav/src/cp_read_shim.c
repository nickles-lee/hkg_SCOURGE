/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * CarPlay iAP2 LD_PRELOAD interposition PROOF shim (VM-only test artifact).
 *
 * Interposes glibc `read@GLIBC_2.2.5` in the REAL /app/bin/iAP2Service-2.0.
 * This is NOT the production CarPlay shim: it does not parse iAP2, does not
 * patch Identify, and never alters or drops traffic. It ONLY calls the real
 * read first (FAIL-OPEN, exact return value + buffer preserved) and appends a
 * bounded trace line to a log file, with an optional caller-page-offset so a
 * hit at the identified seam (call site 0xddd9, return address 0xddde in
 * Transport::recieverThread()) can be recognised even in the PIE guest binary.
 *
 * See docs/hudnav/CP_PROTOCOL.md (the offline ELF finding) and
 * docs/hudnav/CP_PROTOCOL.md (this runtime proof).
 *
 * Build (host): gcc -std=gnu11 -O2 -Wall -Wextra -shared -fPIC \
 *                   -o out/cp_read_shim.so src/cp_read_shim.c -ldl
 * Deploy (VM only): LD_PRELOAD=/tmp/cp-real/cp_read_shim.so \
 *                   CP_READ_LOG=/tmp/cp-real/cp_read_shim.log \
 *                   LD_LIBRARY_PATH=/app/lib:/lib:/usr/lib /app/bin/iAP2Service-2.0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* The guest rootfs is glibc 2.25. glibc >= 2.34 merged libdl into libc and
 * moved dlsym to GLIBC_2.34; a modern-host build would otherwise fail to load
 * at all on the target. Same permanent fix as src/aa_shim.c: pin to 2.2.5.
 *
 * EXTRA TRAP found by the first VM run: on glibc 2.25, dlsym lives in
 * libdl.so.2, NOT libc.so.6. A host link (glibc 2.43, libdl merged) records
 * libc.so.6 as the link-time provider in .gnu.version_r, so the pinned
 * reference produced a fatal load-time relocation error in the guest
 * ("symbol dlsym, version GLIBC_2.2.5 not defined in file libc.so.6").
 * Making the reference WEAK lets ld.so set it to NULL instead of aborting;
 * the wrapper then uses the direct SYS_read syscall, which is exactly what
 * glibc's own read() does. dlsym(RTLD_NEXT,"read") is still the primary path
 * on any system where it resolves. Fail-open in every case. */
extern void *dlsym(void *, const char *) __attribute__((weak));
#if defined(__GLIBC__)
__asm__(".symver dlsym,dlsym@GLIBC_2.2.5");
#endif

typedef ssize_t (*cp_read_fn)(int, void *, size_t);
static cp_read_fn real_read;

/* Bound the trace so a noisy process cannot fill the disposable VM disk. */
#define CP_READ_LOG_MAX 128

static void cp_log_read(int fd, size_t count, ssize_t ret, int saved_errno)
{
    static int logfd = -1;
    static unsigned calls;
    if (calls >= CP_READ_LOG_MAX) return;
    calls++;

    if (logfd < 0) {
        const char *path = getenv("CP_READ_LOG");
        if (!path) path = "/tmp/cp_read_shim.log";
        logfd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (logfd < 0) return;
    }

    /* PIE base is page-aligned; the low 12 bits of a return address are the
     * ELF page offset. The iAP2 seam's call at 0xddd9 returns to 0xddde. */
    uintptr_t ra = (uintptr_t)__builtin_return_address(0);
    char line[320];
    int n = snprintf(line, sizeof line,
                     "CP_READ_HIT n=%u fd=%d count=%zu ret=%zd errno=%d "
                     "caller=%p lo12=0x%03lx%s\n",
                     calls, fd, count, ret, saved_errno,
                     __builtin_return_address(0),
                     (unsigned long)(ra & 0xfffu),
                     (count == 0x101d0u) ? " TRANSPORT_COUNT" : "");
    if (n > 0) {
        ssize_t ignored = write(logfd, line, (size_t)n);
        (void)ignored;
    }
}

ssize_t read(int fd, void *buf, size_t count)
{
    if (!real_read && dlsym) real_read = (cp_read_fn)dlsym(RTLD_NEXT, "read");

    /* Always first; never alter the buffer, the count or the return value. */
    ssize_t ret = real_read ? real_read(fd, buf, count)
                            : syscall(SYS_read, fd, buf, count);
    int saved_errno = errno;
    cp_log_read(fd, count, ret, saved_errno);
    errno = saved_errno;
    return ret;
}
