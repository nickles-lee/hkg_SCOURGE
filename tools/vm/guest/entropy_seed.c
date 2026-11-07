/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/* Research guest infrastructure, not firmware. Run only in the disposable VM.
 * Credit 512 bits from the virtual CPU's RDRAND source to the Linux pool, like
 * a minimal rngd. This models an entropy service, not the unit's RNG hardware.
 * Never credit constants or bytes read from the uninitialized guest urandom.
 */
#include <cpuid.h>
#include <errno.h>
#include <fcntl.h>
#include <immintrin.h>
#include <linux/random.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    unsigned int eax, ebx, ecx, edx;
    unsigned char sample;
    struct {
        int entropy_count;
        int buf_size;
        unsigned long long bytes[8];
    } pool = { .entropy_count = 512, .buf_size = 64, .bytes = {0} };
    int fd;

    if (argc != 2 || strcmp(argv[1], "--rdrand") != 0) {
        fputs("usage: entropy-seed --rdrand (guest only)\n", stderr);
        return 2;
    }
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & bit_RDRND)) {
        puts("HARNESS_BLOCKED virtual CPU RDRAND is unavailable");
        return 77;
    }
    for (unsigned int i = 0; i < 8; ++i) {
        int ok = 0;
        for (unsigned int retry = 0; retry < 128 && !ok; ++retry)
            ok = _rdrand64_step(&pool.bytes[i]);
        if (!ok) {
            memset(&pool, 0, sizeof(pool));
            puts("HARNESS_BLOCKED virtual CPU RDRAND did not yield entropy");
            return 77;
        }
    }
    fd = open("/dev/random", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/random");
        return 1;
    }
    int status = ioctl(fd, RNDADDENTROPY, &pool);
    int saved_errno = errno;
    memset(&pool, 0, sizeof(pool));
    close(fd);
    if (status < 0) {
        errno = saved_errno;
        perror("RNDADDENTROPY");
        return 1;
    }
    puts("HARNESS_SUBSTITUTION cpu-rng-entropy");
    if (syscall(SYS_getrandom, &sample, sizeof(sample), GRND_NONBLOCK) != 1) {
        perror("getrandom nonblocking readiness");
        return 1;
    }
    puts("HARNESS_CHECK entropy-ready PASS");
    return 0;
}
