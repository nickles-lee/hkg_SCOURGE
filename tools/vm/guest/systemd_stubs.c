/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/* Minimal stubs: the simulator never uses systemd socket activation. */
int sd_listen_fds(int unset_environment) { (void)unset_environment; return 0; }
int sd_is_socket(int fd, int family, int type, int listening)
{
    (void)fd; (void)family; (void)type; (void)listening;
    return 0;
}
