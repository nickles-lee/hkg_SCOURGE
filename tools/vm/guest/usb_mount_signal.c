/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/* Emit the Gen5W automount UsbMount signal from the service owner name.
 * usage: usb-mount-signal [mount-point] [device] [fstype]
 */
#include <dbus/dbus.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *mount = argc > 1 ? argv[1] : "/media/usb";
    const char *device = argc > 2 ? argv[2] : "/dev/sda";
    const char *fstype = argc > 3 ? argv[3] : "texfat";
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get(DBUS_BUS_SYSTEM, &err);
    if (conn == NULL) {
        fprintf(stderr, "system bus: %s\n", err.message ? err.message : "failed");
        return 1;
    }
    int rc = dbus_bus_request_name(conn, "com.lge.car.automount",
                                   DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
    if (rc != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
        fprintf(stderr, "name owner rc=%d (%s)\n", rc,
                err.message ? err.message : "not primary");
    DBusMessage *msg = dbus_message_new_signal("/com/lge/car/automount",
                                               "com.lge.car.automount",
                                               "UsbMount");
    dbus_int32_t count = argc > 4 ? (dbus_int32_t)atoi(argv[4]) : 1;
    if (msg == NULL ||
        !dbus_message_append_args(msg, DBUS_TYPE_STRING, &mount,
                                  DBUS_TYPE_INT32, &count,
                                  DBUS_TYPE_STRING, &device,
                                  DBUS_TYPE_STRING, &fstype,
                                  DBUS_TYPE_INVALID)) {
        fprintf(stderr, "signal build failed\n");
        return 1;
    }
    if (!dbus_connection_send(conn, msg, NULL)) {
        fprintf(stderr, "signal send failed\n");
        return 1;
    }
    dbus_connection_flush(conn);
    dbus_message_unref(msg);
    printf("USBMOUNT_SIGNAL %s %s %s\n", mount, device, fstype);
    usleep(750000);
    /* Shared bus connections must be unref'd, not closed. */
    dbus_connection_unref(conn);
    return 0;
}
