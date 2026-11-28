/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Fake "head unit" library for the aa_shim navigation-enable test. It exports, under the exact mangled
 * names of the real liblinux-head-unit.so / libreceiver-lib.so functions, recorders that the shim
 * interposes on and then calls (RTLD_NEXT). Host test only.
 */
#include <stddef.h>

static int calls[8];
static void *last_iface, *last_cb, *last_start_self;

/* counters: 0 phone setter, 1 nav setter, 2 onChannelOpened real, 3 start, 4 slots called by nav setter */
int fake_get(int i) { return calls[i]; }
void *fake_last_iface(void) { return last_iface; }
void *fake_last_cb(void) { return last_cb; }
void *fake_last_start_self(void) { return last_start_self; }

void _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(void *iface, void *cb)
{
    (void)iface; (void)cb; calls[0]++;
}

void _ZN21CAndroidAutoInterface37setInstanceINavigationStatusServiceCBEP26INavigationStatusServiceCB(void *iface, void *cb)
{
    calls[1]++; last_iface = iface; last_cb = cb;
    /* The real library later calls the callback's vtable slots 0x10 / 0x18 / 0x20 (CEventThread). Do the same. */
    if (cb) {
        void **vt = *(void ***)cb;
        ((void (*)(void *, void *))vt[2])(cb, NULL); calls[4]++;
        ((void (*)(void *, void *))vt[3])(cb, NULL); calls[4]++;
        ((void (*)(void *, int))vt[4])(cb, 7);       calls[4]++;
    }
}

void _ZN20ProtocolEndpointBase15onChannelOpenedEh(void *self, unsigned char channel)
{
    (void)self; (void)channel; calls[2]++;
}

/* libreceiver's `vtable for NavigationStatusEndpoint`: a nav endpoint's vptr is symbol + 0x10 */
void *_ZTV24NavigationStatusEndpoint[10];

void _ZN24NavigationStatusEndpoint5startEv(void *self)
{
    calls[3]++; last_start_self = self;
}
