/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_usb.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define CP_USB_APPLE_VENDOR "05ac"

bool cp_usb_apple_present(const char *sysfs_root)
{
    DIR *dir;
    struct dirent *entry;
    bool found = false;

    if (sysfs_root == NULL || sysfs_root[0] == '\0') return true;
    dir = opendir(sysfs_root);
    if (dir == NULL) return true;

    for (;;) {
        char path[512];
        FILE *file;
        char vendor[16];
        int length;

        errno = 0;
        entry = readdir(dir);
        if (entry == NULL) {
            if (errno != 0) found = true; /* readdir failure: fail closed */
            break;
        }
        if (entry->d_name[0] == '.') continue;
        length = snprintf(path, sizeof(path), "%s/%s/idVendor",
                          sysfs_root, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            found = true; /* truncated path could hide a device */
            break;
        }
        file = fopen(path, "r");
        if (file == NULL) {
            /* Interface children (name contains ':') have no idVendor.
             * Any other missing/unreadable entry could hide a device. */
            if ((errno == ENOENT || errno == ENOTDIR) &&
                strchr(entry->d_name, ':') != NULL) continue;
            found = true;
            break;
        }
        if (fgets(vendor, sizeof(vendor), file) == NULL) {
            found = true; /* empty/unreadable idVendor: fail closed */
        } else if (strncmp(vendor, CP_USB_APPLE_VENDOR, 4) == 0) {
            found = true;
        }
        fclose(file);
        if (found) break;
    }
    closedir(dir);
    return found;
}
