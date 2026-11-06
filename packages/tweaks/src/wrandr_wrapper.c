/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/*
 * wrandr wrapper for the Ioniq 5 Tweaks app.
 *
 * The vendor tints the front display with:
 *   wrandr --crtc=1 --brightness=<r:g:b>
 * (ApplicationManager setFrontBrightnessRGBValue) and resets it to
 * --brightness=128:128:128 when the blue-light filter is disabled, which
 * CarPlay/Android Auto trigger.  If the Tweaks infotainment fix is enabled,
 * replace only that neutral brightness with the current warmth.  Everything
 * else goes straight to the real wrandr binary.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define REAL_WRANDR "/usr/bin/wrandr.real"
#define GAMMA_FILE "/rw_data/tweaks/cluster_gamma"
#define DEFAULT_FRONT_CRTC 1

const char *tweaks_wrandr_marker = "TWEAKS_WRANDR_WRAPPER";

static const char *gamma_file(void)
{
    const char *rw = getenv("TWEAKS_RW_DIR");
    static char path[512];
    if (rw && *rw) {
        snprintf(path, sizeof(path), "%s/cluster_gamma", rw);
        return path;
    }
    return GAMMA_FILE;
}

static int read_front_crtc(void)
{
    const char *rw = getenv("TWEAKS_RW_DIR");
    const char *env = getenv("TWEAKS_FRONT_CRTC");
    char path[512], line[256];
    FILE *f;
    int crtc = DEFAULT_FRONT_CRTC;

    if (env && *env)
        return atoi(env);
    if (rw && *rw)
        snprintf(path, sizeof(path), "%s/config.conf", rw);
    else
        snprintf(path, sizeof(path), "/rw_data/tweaks/config.conf");
    f = fopen(path, "r");
    if (!f)
        return crtc;
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "main_crtc=", 10)) {
            crtc = atoi(line + 10);
            break;
        }
    }
    fclose(f);
    return crtc;
}

static int read_desired(char *buf, size_t size)
{
    FILE *f = fopen(gamma_file(), "r");
    size_t n;
    if (!f)
        return -1;
    n = fread(buf, 1, size - 1, f);
    fclose(f);
    buf[n] = 0;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
        buf[--n] = 0;
    if (!strcmp(buf, "off") || !buf[0])
        return -1;
    return 0;
}

static int parse_crtc(const char *arg)
{
    if (!strncmp(arg, "--crtc=", 7))
        return atoi(arg + 7);
    return -1;
}

int main(int argc, char **argv)
{
    char desired[64];
    char brightness_arg[80];
    char *new_argv[64];
    const char *real = getenv("TWEAKS_REAL_WRANDR");
    int i, out = 0;
    int front_crtc = read_front_crtc();
    int crtc = -1;
    int have_crtc = 0;
    int is_neutral = 0;
    int have_brightness = 0;

    if (!real || !*real)
        real = REAL_WRANDR;

    for (i = 1; i < argc; i++) {
        int c = parse_crtc(argv[i]);
        if (c >= 0) {
            crtc = c;
            have_crtc = 1;
        }
        if (!strncmp(argv[i], "--brightness=", 13)) {
            have_brightness = 1;
            if (!strcmp(argv[i] + 13, "128:128:128"))
                is_neutral = 1;
        }
    }

    /*
     * Only rewrite a neutral brightness that explicitly targets the front
     * display.  A command without --crtc= must pass through untouched,
     * otherwise a vendor call for another display could be tinted amber.
     */
    if (!have_crtc || crtc != front_crtc || !have_brightness || !is_neutral ||
        read_desired(desired, sizeof(desired)) < 0) {
        execv(real, argv);
        perror("wrandr wrapper: execv");
        return 127;
    }

    snprintf(brightness_arg, sizeof(brightness_arg), "--brightness=%s", desired);
    for (i = 0; i < argc && out < (int)(sizeof(new_argv) / sizeof(new_argv[0])) - 1; i++) {
        if (!strncmp(argv[i], "--brightness=", 13))
            new_argv[out++] = brightness_arg;
        else
            new_argv[out++] = argv[i];
    }
    new_argv[out] = NULL;

    execv(real, new_argv);
    perror("wrandr wrapper: execv");
    return 127;
}
