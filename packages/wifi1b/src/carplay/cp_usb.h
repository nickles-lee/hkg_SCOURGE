/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Conservative Apple-USB presence check for the experimental CarPlay broker.
 * Any enumerated Apple USB device blocks the wireless offer.  This is
 * intentionally broader than "wired CarPlay is active": a charging iPhone
 * must not race a wired session.  An unreadable/oversized root entry or an
 * idVendor read failure fails closed.  The scan is point-in-time; the broker
 * repeats it during its periodic policy checks.
 */
#ifndef CP_USB_H
#define CP_USB_H

#include <stdbool.h>

/* sysfs_root is normally /sys/bus/usb/devices.  A missing/unreadable root
 * returns true (block), never false. */
bool cp_usb_apple_present(const char *sysfs_root);

#endif /* CP_USB_H */
