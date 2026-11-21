/*
 * SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host-side test for the PCL read fallback: an app whose pclInitLibrary
 * succeeds but whose status read is empty must not be selected.
 */
#include <stdio.h>
#include <string.h>

extern int persist_test_read(void);

static const char *current_app;
static int scenario;
static int status_reads;

int pclInitLibrary(const char *app, int mode)
{
    (void)mode;
    current_app = app;
    return 0;
}

int pclDeinitLibrary(void)
{
    return 0;
}

static const char *value_for(const char *key)
{
    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_STATUS"))
        return "1";
    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_WARM_LEVEL"))
        return "5";
    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_START_TIME"))
        return "420";
    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_END_TIME"))
        return "1080";
    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_LAST_SETTING_MODE"))
        return "0";
    if (!strcmp(key, "APP_SETTING_DISP_GAMMA_R_G_B"))
        return "128:128:128";
    if (!strcmp(key, "APP_SETTING_DISP_BRIGHTNESS_R_G_B"))
        return "128:116:104";
    return NULL;
}

int pclKeyReadData(unsigned int ldbid, const char *key, unsigned int user,
                   unsigned int seat, unsigned char *buffer, int size)
{
    const char *value;
    (void)ldbid;
    (void)user;
    (void)seat;

    if (!strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_STATUS"))
        status_reads++;

    if (scenario == 0) {
        /* TimeManager initializes but serves an empty status; AppSetup has
         * the real values.  The helper must fall through to AppSetup. */
        if (!strcmp(current_app, "TimeManager"))
            return !strcmp(key, "APP_SETTING_DISP_BLUE_LIGHT_STATUS") ? 0 : -6;
        if (strcmp(current_app, "AppSetup") != 0)
            return -6;
    } else {
        /* TimeManager is the working owner. */
        if (strcmp(current_app, "TimeManager") != 0)
            return -6;
    }

    value = value_for(key);
    if (!value)
        return -6;
    snprintf((char *)buffer, (size_t)size, "%s", value);
    return (int)strlen(value);
}

int pclKeyWriteData(unsigned int ldbid, const char *key, unsigned int user,
                    unsigned int seat, unsigned char *buffer, int size)
{
    (void)ldbid;
    (void)key;
    (void)user;
    (void)seat;
    (void)buffer;
    (void)size;
    return 0;
}

int main(void)
{
    int rc;

    scenario = 0;
    status_reads = 0;
    rc = persist_test_read();
    /* TimeManager (empty) + AppSetup (status=1) must both be probed. */
    if (rc != 0 || status_reads != 2) {
        fprintf(stderr, "fallback failed rc=%d status_reads=%d\n",
                rc, status_reads);
        return 1;
    }

    scenario = 1;
    status_reads = 0;
    rc = persist_test_read();
    if (rc != 0 || status_reads != 1) {
        fprintf(stderr, "TimeManager selection failed rc=%d status_reads=%d\n",
                rc, status_reads);
        return 1;
    }

    puts("PASS: PCL read app fallback");
    return 0;
}
