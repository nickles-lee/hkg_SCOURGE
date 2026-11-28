/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host probe for the CarPlay LD_PRELOAD hook (src/cp_hook.c). Run with
 * LD_PRELOAD=out/cp_hook.so CP_HOOK_FORCE=1. Uses a pipe as a stand-in for
 * /dev/ttyIAP20 and a unix socket as a stand-in for hudnavd.
 *
 * argv[1] = unix socket path to create and listen on.
 * Exit 0 = all checks passed.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void put16(uint8_t *b, uint16_t v) { b[0] = (uint8_t)(v >> 8); b[1] = (uint8_t)v; }

static size_t put_tlv(uint8_t *b, size_t p, uint16_t id, const uint8_t *v, size_t vn)
{
    put16(b + p, (uint16_t)(vn + 4)); put16(b + p + 2, id);
    memcpy(b + p + 4, v, vn); return p + vn + 4;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "usage: cp_hook_probe <socket-path>\n"); return 2; }

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) return 2;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, argv[1], sizeof a.sun_path - 1);
    unlink(argv[1]);
    if (bind(srv, (struct sockaddr *)&a, sizeof a) < 0 || listen(srv, 1) < 0) return 2;
    setenv("HUDNAV_SOCK", argv[1], 1);
    setenv("CP_HOOK_FORCE", "1", 1);

    int fds[2];
    CHECK(pipe(fds) == 0);
    char fdbuf[16];
    snprintf(fdbuf, sizeof fdbuf, "%d", fds[1]);
    setenv("CP_HOOK_FD", fdbuf, 1);

    /* Synthetic Identify link body: [02][session][40 40 len 1D01 TLV6 TLV7]. */
    uint8_t id[128]; size_t p = 2 + 6;
    const uint8_t s6[2] = { 0x50, 0x00 };
    const uint8_t s7[2] = { 0x50, 0x01 };
    p = put_tlv(id, p, 6, s6, sizeof s6);
    p = put_tlv(id, p, 7, s7, sizeof s7);
    id[0] = 0x02; id[1] = 0x33; id[2] = 0x40; id[3] = 0x40;
    put16(id + 4, (uint16_t)(p - 2)); put16(id + 6, 0x1d01);
    CHECK(write(fds[1], id, p) == (ssize_t)p);

    usleep(20000);
    uint8_t got[512];
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    ssize_t gn = read(fds[0], got, sizeof got);
    CHECK(gn > (ssize_t)p);                         /* Identify + Start frame */

    /* Walk the two link bodies in the pipe output. */
    size_t o = 0;
    int frames = 0;
    while (o + 8 <= (size_t)gn) {
        CHECK(got[o] == 0x02 && got[o + 1] == 0x33);
        size_t base = o + 2;
        CHECK(got[base] == 0x40 && got[base + 1] == 0x40);
        uint16_t mlen = (uint16_t)((got[base + 2] << 8) | got[base + 3]);
        uint16_t msgid = (uint16_t)((got[base + 4] << 8) | got[base + 5]);
        if (frames == 0) {
            CHECK(msgid == 0x1d01);
            CHECK(memmem(got + base, mlen, "\x52\x00", 2) != NULL);
            CHECK(memmem(got + base, mlen, "\x52\x03", 2) != NULL);
            CHECK(memmem(got + base, mlen, "\x52\x01", 2) != NULL);
            CHECK(memmem(got + base, mlen, "\x52\x02", 2) != NULL);
            CHECK(memmem(got + base, mlen, "\x52\x04", 2) != NULL);
            CHECK(memmem(got + base, mlen, "\x00\x1e", 2) != NULL);
        } else {
            CHECK(msgid == 0x5200 && mlen == 6);
            CHECK(memcmp(got + base, "\x40\x40\x00\x06\x52\x00", 6) == 0);
        }
        frames++;
        o = base + mlen;
    }
    CHECK(frames == 2);

    /* A second Identify must NOT emit another 0x5200. */
    CHECK(write(fds[1], id, p) == (ssize_t)p);
    usleep(20000);
    gn = read(fds[0], got, sizeof got);
    CHECK(gn > (ssize_t)p && gn < 2 * (ssize_t)p);  /* patched Identify only */
    /* no second 0x5200 frame */
    size_t v = 0;
    while (v + 8 <= (size_t)gn) {
        uint16_t mid = (uint16_t)((got[v + 6] << 8) | got[v + 7]);
        CHECK(mid != 0x5200);
        v += 2 + ((got[v + 4] << 8) | got[v + 5]);
    }

    /* Synthetic 0x5201 Update with the 2-byte link prefix: the read hook must
     * see it (count 0x101d0) and forward normalized lines to the socket. */
    uint8_t up[64]; size_t q = 2 + 6;
    q = put_tlv(up, q, 0x01, (const uint8_t *)"\x01", 1);
    q = put_tlv(up, q, 0x0a, (const uint8_t *)"\x00\x00\x00\x64", 4);
    q = put_tlv(up, q, 0x14, (const uint8_t *)"\x01", 1);
    up[0] = 0x02; up[1] = 0x33; up[2] = 0x40; up[3] = 0x40;
    put16(up + 4, (uint16_t)(q - 2)); put16(up + 6, 0x5201);
    CHECK(write(fds[1], up, q) == (ssize_t)q);
    usleep(20000);
    uint8_t *rbuf = malloc(0x101d0);
    ssize_t rn = read(fds[0], rbuf, 0x101d0);      /* hooked: count == 0x101d0 */
    CHECK(rn == (ssize_t)q);
    free(rbuf);

    fcntl(srv, F_SETFL, O_NONBLOCK);
    int conn = -1;
    for (int i = 0; i < 50 && conn < 0; i++) { usleep(10000); conn = accept(srv, NULL, NULL); }
    CHECK(conn >= 0);
    if (conn >= 0) {
        char lines[512]; ssize_t ln = read(conn, lines, sizeof lines - 1);
        if (ln > 0) lines[ln] = '\0'; else lines[0] = '\0';
        CHECK(strstr(lines, "STATE s=1") != NULL);
        CHECK(strstr(lines, "DIST m=100") != NULL);
        CHECK(strstr(lines, "HB") != NULL);
        close(conn);
    }
    /* Phase 2: the REAL open path (no CP_HOOK_FD override). iAP2Service holds TWO /dev/ttyIAP20 fds;
     * the hook once remembered only the last one, so an Identify written on the other fd was never
     * patched (first live CarPlay test on the car, 2026-11-25). Write on the FIRST-opened fd. */
    unsetenv("CP_HOOK_FD");
    char tp[96];
    snprintf(tp, sizeof tp, "/tmp/hudnav-probe-ttyIAP20.%d", (int)getpid());
    int fa = open(tp, O_RDWR | O_CREAT | O_TRUNC, 0600);
    int fb = open(tp, O_RDWR);
    CHECK(fa >= 0 && fb >= 0 && fa != fb);
    CHECK(write(fa, id, p) == (ssize_t)p);               /* caller still sees a complete write */
    off_t fsz = lseek(fa, 0, SEEK_END);
    CHECK(fsz > (off_t)p);                               /* ... but the Identify on the wire is patched */
    off_t zero = lseek(fb, 0, SEEK_END);
    (void)zero;
    close(fa); close(fb); unlink(tp);

    close(srv); close(fds[0]); close(fds[1]); unlink(argv[1]);

    printf("cp hook: %d checks, %d failures\n", checks, fails);
    return fails ? 1 : 0;
}
