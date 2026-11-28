/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Canary for the /etc/ld.so.preload rehearsal: a library that does NOTHING and
 * needs nothing (no libc, no versioned symbols, no constructors). If a global
 * preload of even this breaks boot, the mechanism itself is the problem, not
 * our shim. Build: gcc -shared -fPIC -nostdlib -o noop.so noop.c
 */
int hudnav_canary_noop = 1;
