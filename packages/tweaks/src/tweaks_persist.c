/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */
/*
 * tweaks_persist - small dynamic helper for the vendor GENIVI persistence
 * client library.  The main Tweaks daemon is static; this helper is linked
 * against the head unit's libc and PCL so it can read and write the vehicle
 * settings.
 *
 * pclKeyWriteData enforces a per-resource owner check: the write is
 * rejected with -44 ("not the responsible application") unless the app
 * name passed to pclInitLibrary matches the resource's owner in the
 * Resource Configuration Table.  The write mode therefore retries under
 * each candidate app name and reports the PCL return codes on stderr.
 *
 * Usage:
 *   tweaks_persist read
 *   tweaks_persist write <key> <value>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int pclInitLibrary(const char *appname, int shutdown_mode);
extern int pclDeinitLibrary(void);
extern int pclKeyReadData(unsigned int ldbid, const char *resource_id,
                          unsigned int user_no, unsigned int seat_no,
                          unsigned char *buffer, int buffer_size);
extern int pclKeyWriteData(unsigned int ldbid, const char *resource_id,
                           unsigned int user_no, unsigned int seat_no,
                           unsigned char *buffer, int buffer_size);

static const char *READ_APPS[] = {"AppSetup", "AppMediaPlayer", "ApplicationManager",
                                  "AppHomeScreen", "tweaks_daemon", NULL};
/* Owners to try for writes, most likely first. */
static const char *WRITE_APPS[] = {"AppSetup", "TimeManager", "ApplicationManager",
                                   "AppHomeScreen", "AppMediaPlayer",
                                   "tweaks_daemon", NULL};

/* The name of the app whose write succeeded last time, next to the helper. */
static char cache_path[512];

static void set_cache_path(const char *argv0)
{
    const char *rw = getenv("TWEAKS_RW_DIR");

    if (rw && *rw) {
        snprintf(cache_path, sizeof(cache_path), "%s/pcl_write_app", rw);
        return;
    }
    if (argv0 && strchr(argv0, '/')) {
        char tmp[512];
        char *slash;
        snprintf(tmp, sizeof(tmp), "%s", argv0);
        slash = strrchr(tmp, '/');
        *slash = 0;
        snprintf(cache_path, sizeof(cache_path), "%s/pcl_write_app", tmp);
        return;
    }
    snprintf(cache_path, sizeof(cache_path), "/rw_data/tweaks/pcl_write_app");
}

static void read_cached_app(char *out, size_t size)
{
    FILE *f;

    out[0] = 0;
    f = fopen(cache_path, "r");
    if (!f)
        return;
    if (fgets(out, size, f)) {
        size_t n = strlen(out);
        while (n && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = 0;
    }
    fclose(f);
    /* Only accept names from the candidate list. */
    for (int i = 0; WRITE_APPS[i]; i++) {
        if (!strcmp(out, WRITE_APPS[i]))
            return;
    }
    out[0] = 0;
}

static void write_cached_app(const char *app)
{
    FILE *f = fopen(cache_path, "w");

    if (!f)
        return;
    fprintf(f, "%s\n", app);
    fclose(f);
}

static int read_key(const char *key)
{
    unsigned char buf[512];
    int n;
    memset(buf, 0, sizeof(buf));
    n = pclKeyReadData(0, key, 0, 0, buf, (int)sizeof(buf) - 1);
    if (n < 0) {
        printf("%s=\n", key);
        return -1;
    }
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    buf[n] = 0;
    /* Strip trailing CR/LF. */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = 0;
    printf("%s=%s\n", key, buf);
    return 0;
}

static int do_read(void)
{
    int r = -1;
    for (int i = 0; READ_APPS[i]; i++) {
        r = pclInitLibrary(READ_APPS[i], 0);
        if (r >= 0) {
            fprintf(stderr, "pcl read app=%s\n", READ_APPS[i]);
            break;
        }
        fprintf(stderr, "pclInitLibrary(%s) failed: %d\n", READ_APPS[i], r);
    }
    if (r < 0) {
        fprintf(stderr, "pclInitLibrary failed for every candidate\n");
        return 3;
    }
    read_key("APP_SETTING_DISP_BLUE_LIGHT_STATUS");
    read_key("APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL");
    read_key("APP_SETTING_DISP_GAMMA_R_G_B");
    read_key("APP_SETTING_DISP_BRIGHTNESS_R_G_B");
    read_key("APP_SETTING_CLOCK_DAYLIGHTSAVETIME");
    read_key("APP_SETTING_CLOCK_TIMEZONE");
    read_key("APP_SETTING_CLOCK_GPS");
    read_key("APP_TIMEMANAGER_GPS_OFFSET");
    return 0;
}

static int do_write(const char *key, const char *value)
{
    unsigned char *buf = (unsigned char *)value;
    int len = (int)strlen(value);
    int last_rc = -99;
    const char *last_app = "";
    const char *order[16];
    int norder = 0;
    char cached[64];

    /*
     * Try the app name whose write succeeded last time first, so the normal
     * case is a single init + write.  The remaining candidates follow in
     * case the cached one stops working.
     */
    read_cached_app(cached, sizeof(cached));
    if (cached[0])
        order[norder++] = cached;
    for (int i = 0; WRITE_APPS[i] && norder < 16; i++)
        if (!cached[0] || strcmp(WRITE_APPS[i], cached) != 0)
            order[norder++] = WRITE_APPS[i];

    fprintf(stderr, "pcl write cached_app=%s candidates:", cached[0] ? cached : "(none)");
    for (int i = 0; i < norder; i++)
        fprintf(stderr, " %s", order[i]);
    fprintf(stderr, "\n");

    for (int i = 0; i < norder; i++) {
        const char *app = order[i];
        int r = pclInitLibrary(app, 0);
        if (r < 0) {
            fprintf(stderr, "pclInitLibrary(%s) failed: %d\n", app, r);
            continue;
        }
        fprintf(stderr, "pclInitLibrary(%s) ok\n", app);
        r = pclKeyWriteData(0, key, 0, 0, buf, len);
        if (r >= 0) {
            /*
             * Read the key back in the SAME registration.  This is the
             * authoritative confirmation: a read from a different
             * registration can hit a stale per-connection cache and report
             * the old value even though the write landed.
             */
            unsigned char rb[512];
            char val[64] = "";
            int rn = pclKeyReadData(0, key, 0, 0, rb, sizeof(rb) - 1);
            if (rn >= 0) {
                int vn = rn < (int)sizeof(rb) - 1 ? rn : (int)sizeof(rb) - 1;
                rb[vn] = 0;
                while (vn > 0 && (rb[vn - 1] == '\n' || rb[vn - 1] == '\r'))
                    rb[--vn] = 0;
                snprintf(val, sizeof(val), "%s", (const char *)rb);
            }
            fprintf(stderr, "pclKeyWriteData(%s) ok as %s rc=%d\n", key, app, r);
            printf("%s=%s\n", key, value);
            printf("verify=%s\n", val[0] ? val : "(unreadable)");
            if (!cached[0] || strcmp(app, cached) != 0)
                write_cached_app(app);
            pclDeinitLibrary();
            return 0;
        }
        fprintf(stderr, "pclKeyWriteData(%s) failed as %s rc=%d\n", key, app, r);
        last_rc = r;
        last_app = app;
        pclDeinitLibrary();
    }
    printf("write_failed rc=%d app=%s\n", last_rc, last_app);
    return 4;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s read|write <key> <value>\n", argv[0]);
        return 2;
    }

    set_cache_path(argv[0]);

    if (strcmp(argv[1], "read") == 0) {
        int r = do_read();
        if (r == 0)
            pclDeinitLibrary();
        return r;
    }
    if (strcmp(argv[1], "write") == 0 && argc >= 4)
        return do_write(argv[2], argv[3]);

    fprintf(stderr, "usage: %s read|write <key> <value>\n", argv[0]);
    return 2;
}
