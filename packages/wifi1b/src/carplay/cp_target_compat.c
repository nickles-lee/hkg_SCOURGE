/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * TARGET: Gen5W / IONIQ 5, x86-64, target rootfs glibc 2.25
 *
 * Link-time compatibility shims for the experimental CarPlay broker helper.
 * The helper is compiled with host headers but linked against the target
 * rootfs libc 2.25.  Two host-header ABI redirects would otherwise reference
 * symbols the target libc does not export:
 *
 * 1) fstat/fstatat.  Host glibc headers emit plain fstat/fstatat references
 *    (real symbols since glibc 2.33).  Target glibc 2.25 exports only the
 *    xstat family: __fxstat@@GLIBC_2.2.5 and __fxstatat@@GLIBC_2.4.  On
 *    x86-64 uses _STAT_VER 1 for the kernel struct stat.  Target
 *    disassembly shows __fxstat rejects versions above 1 with EINVAL, then
 *    issues the raw fstat syscall with the caller's buffer unchanged;
 *    __fxstatat does the same
 *    with newfstatat and forwards the flags argument verbatim, preserving
 *    AT_SYMLINK_NOFOLLOW.  These wrappers are pure pass-throughs; they do not
 *    weaken the symlink/inode/uid checks in cp_spp_socket.c.
 *
 * 2) __isoc23_strtol/__isoc23_strtoul.  Host headers redirect strtol/strtoul
 *    whenever _GNU_SOURCE is defined (features.h defines _ISOC23_SOURCE, so
 *    -std=c11/gnu11 cannot turn it off).  Target glibc 2.25 has no
 *    __isoc23_* symbols and no 0b-prefix semantics.  Forward to the target's
 *    own strtol/strtoul so config parsing keeps the target's pre-C23
 *    behaviour; a "0b..." value fails closed.
 *
 * This file deliberately does not include <stdlib.h>, so the host redirect
 * cannot recurse inside the shims.  It is linked only into the target helper;
 * it is not part of the Wifi1b daemon or any installer payload.
 */
#include <sys/stat.h>

/* Target glibc 2.25 xstat entry points.  Not declared by current host
 * headers, which use the post-2.33 plain names instead. */
extern int __fxstat(int version, int fd, struct stat *buf);
extern int __fxstatat(int version, int fd, const char *restrict path,
                      struct stat *restrict buf, int flags);

/* x86-64 uses version 1 for the kernel struct stat.  Target libc accepts
 * versions 0/1 here; 1 is the layout this build relies on. */
#define CP_TARGET_STAT_VER 1

int fstat(int fd, struct stat *buf)
{
    return __fxstat(CP_TARGET_STAT_VER, fd, buf);
}

int fstatat(int fd, const char *restrict path, struct stat *restrict buf,
            int flags)
{
    return __fxstatat(CP_TARGET_STAT_VER, fd, path, buf, flags);
}

/* Plain target declarations; no host <stdlib.h> redirect. */
extern long int strtol(const char *restrict nptr, char **restrict endptr,
                       int base);
extern unsigned long int strtoul(const char *restrict nptr,
                                 char **restrict endptr, int base);

long int __isoc23_strtol(const char *restrict nptr, char **restrict endptr,
                         int base)
{
    return strtol(nptr, endptr, base);
}

unsigned long int __isoc23_strtoul(const char *restrict nptr,
                                   char **restrict endptr, int base)
{
    return strtoul(nptr, endptr, base);
}
