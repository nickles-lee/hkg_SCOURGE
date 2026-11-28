/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host probe for the aa_shim navigation-enable interposers. Run with
 *   LD_PRELOAD=out/aa_shim.so LD_LIBRARY_PATH=out HUDNAV_FORCE=1 \
 *   HUDNAV_AA_NAV_ENABLE_FILE=<f> HUDNAV_AA_TRACE_FILE=<t> HUDNAV_AA_TRACE_LOG=<l> aa_nav_probe [inert]
 * against out/libaafake.so (tests/aa_nav_fake.c). "inert" = run without HUDNAV_FORCE (the shim must do nothing).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

extern int fake_get(int);
extern void *fake_last_iface(void), *fake_last_cb(void), *fake_last_start_self(void);
extern void _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(void *, void *);
extern void _ZN20ProtocolEndpointBase15onChannelOpenedEh(void *, unsigned char);
extern void *_ZTV24NavigationStatusEndpoint[10];

static int fails = 0, checks = 0;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

int main(int argc, char **argv)
{
    int inert = argc > 1 && !strcmp(argv[1], "inert");
    const char *enable = getenv("HUDNAV_AA_NAV_ENABLE_FILE");
    if (!enable) { fprintf(stderr, "need HUDNAV_AA_NAV_ENABLE_FILE\n"); return 2; }
    unlink(enable);

    int iface_obj = 0, cb_obj = 0;
    void *nav_self = &iface_obj;                    /* object whose vptr we set below */
    void *nav_obj[2]; nav_obj[0] = (char *)_ZTV24NavigationStatusEndpoint + 0x10;
    void *other_obj[2]; other_obj[0] = (void *)&cb_obj;     /* some other endpoint class */
    (void)nav_self;

    /* 1. enable file absent: originals run, nothing added */
    _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(&iface_obj, &cb_obj);
    _ZN20ProtocolEndpointBase15onChannelOpenedEh(nav_obj, 3);
    CHECK(fake_get(0) == 1);                         /* the real PhoneStatus setter ran */
    CHECK(fake_get(1) == 0);                         /* nav setter NOT called */
    CHECK(fake_get(2) == 1 && fake_get(3) == 0);     /* real onChannelOpened ran, start NOT called */

    /* 2. enable file present */
    int fd = open(enable, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd);
    _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(&iface_obj, &cb_obj);
    if (inert) {
        CHECK(fake_get(1) == 0);
        _ZN20ProtocolEndpointBase15onChannelOpenedEh(nav_obj, 3);
        CHECK(fake_get(3) == 0);
    } else {
        CHECK(fake_get(0) == 2);
        CHECK(fake_get(1) == 1);                     /* nav setter called exactly once */
        CHECK(fake_last_iface() == (void *)&iface_obj);
        CHECK(fake_last_cb() != NULL);
        CHECK(fake_get(4) == 3);                     /* the three vtable slots are callable no-ops (no crash) */
        /* every later registration (e.g. a rebuilt interface on the next phone connection) is covered too */
        _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(&iface_obj, &cb_obj);
        CHECK(fake_get(1) == 2);
        CHECK(fake_get(4) == 6);                     /* and the stand-in's slots stay callable */
        /* a NULL interface must be ignored (original still runs) */
        _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(NULL, &cb_obj);
        CHECK(fake_get(1) == 2);

        /* start() only for the navigation endpoint, only after the real onChannelOpened */
        _ZN20ProtocolEndpointBase15onChannelOpenedEh(other_obj, 5);
        CHECK(fake_get(3) == 0);
        _ZN20ProtocolEndpointBase15onChannelOpenedEh(nav_obj, 3);
        CHECK(fake_get(3) == 1 && fake_last_start_self() == (void *)nav_obj);
        CHECK(fake_get(2) == 3);                     /* real onChannelOpened ran for every call so far (1+2) */

        /* 3. removing the enable file switches it off again */
        unlink(enable);
        _ZN20ProtocolEndpointBase15onChannelOpenedEh(nav_obj, 3);
        CHECK(fake_get(3) == 1);
    }
    unlink(enable);
    printf("aa nav probe%s: %d checks, %d failures\n", inert ? " (inert)" : "", checks, fails);
    return fails ? 1 : 0;
}
