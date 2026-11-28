/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Host probe for the aa_shim process self-gate. Run with LD_PRELOAD pointing at
 * out/aa_shim.so; it calls the interposed navigationStatusCallback once. Whether
 * the shim forwards a line to HUDNAV_SOCK is decided entirely by the gate.
 * The executable basename here is `aa_gate_probe`, so the default must be inert.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>

typedef void (*status_fn)(void *self, int status);

int main(void)
{
    status_fn f = (status_fn)dlsym(RTLD_DEFAULT,
        "_ZN24NavigationStatusCallback24navigationStatusCallbackEi");
    if (!f) {
        fprintf(stderr, "aa_gate_probe: interposed symbol not found\n");
        return 2;
    }
    f(NULL, 1);
    return 0;
}
