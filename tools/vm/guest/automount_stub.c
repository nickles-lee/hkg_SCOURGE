/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Persistent system-bus stand-in for com.lge.car.automount.
 * Emits UsbMount/UsbUnMount on request so both AppDMClient and the update
 * engine see a stable service owner. Research infrastructure.
 *
 * usage: automount-stub [mount-point] [device] [fstype]
 */
#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUS_NAME "com.lge.car.automount"
#define PATH     "/com/lge/car/automount"
#define IFACE    "com.lge.car.automount"

static volatile sig_atomic_t keep_running = 1;
static char mount_point[512] = "/media/usb";
static char device[256] = "/dev/sda";
static char fstype[64] = "ext4";

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static int send_message(DBusConnection *connection, DBusMessage *reply)
{
    if (reply == NULL)
        return 0;
    if (!dbus_connection_send(connection, reply, NULL)) {
        dbus_message_unref(reply);
        return 0;
    }
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
    return 1;
}

static void emit(DBusConnection *connection, const char *member)
{
    DBusMessage *msg = dbus_message_new_signal(PATH, IFACE, member);
    dbus_int32_t count = 1;
    const char *mp = mount_point;
    const char *dev = device;
    const char *fs = fstype;
    if (msg == NULL)
        return;
    if (!dbus_message_append_args(msg, DBUS_TYPE_STRING, &mp,
                                  DBUS_TYPE_INT32, &count,
                                  DBUS_TYPE_STRING, &dev,
                                  DBUS_TYPE_STRING, &fs,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return;
    }
    if (dbus_connection_send(connection, msg, NULL))
        dbus_connection_flush(connection);
    dbus_message_unref(msg);
    fprintf(stderr, "AUTOMOUNT_STUB_EMIT %s %s %s %s\n", member, mount_point,
            device, fstype);
    fflush(stderr);
}

int main(int argc, char **argv)
{
    DBusConnection *connection;
    DBusError error;
    int request_result;

    if (argc > 1) snprintf(mount_point, sizeof(mount_point), "%s", argv[1]);
    if (argc > 2) snprintf(device, sizeof(device), "%s", argv[2]);
    if (argc > 3) snprintf(fstype, sizeof(fstype), "%s", argv[3]);

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection == NULL) {
        fprintf(stderr, "automount-stub: system bus: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return 2;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    request_result = dbus_bus_request_name(connection, BUS_NAME,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "automount-stub: request_name: %s: %s\n",
                error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 3;
    }
    if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "automount-stub: name busy rc=%d\n", request_result);
        dbus_connection_unref(connection);
        return 4;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    fprintf(stderr, "AUTOMOUNT_STUB_READY %s %s %s\n", mount_point, device,
            fstype);
    fflush(stderr);
    emit(connection, "UsbMount");

    while (keep_running && dbus_connection_get_is_connected(connection)) {
        DBusMessage *request;
        const char *member;

        dbus_connection_read_write(connection, 500);
        request = dbus_connection_pop_message(connection);
        if (request == NULL)
            continue;
        if (dbus_message_get_type(request) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
            dbus_message_unref(request);
            continue;
        }
        member = dbus_message_get_member(request);
        fprintf(stderr, "AUTOMOUNT_STUB_CALL member=%s signature=%s\n",
                member ? member : "(null)",
                dbus_message_get_signature(request));
        fflush(stderr);
        if (dbus_message_is_method_call(request, IFACE, "trigger_mount") ||
            dbus_message_is_method_call(request, IFACE, "UsbMount")) {
            emit(connection, "UsbMount");
            send_message(connection, dbus_message_new_method_return(request));
        } else if (dbus_message_is_method_call(request, IFACE, "trigger_unmount") ||
                   dbus_message_is_method_call(request, IFACE, "UsbUnMount")) {
            emit(connection, "UsbUnMount");
            send_message(connection, dbus_message_new_method_return(request));
        } else if (dbus_message_is_method_call(
                       request, "org.freedesktop.DBus.Introspectable",
                       "Introspect")) {
            const char *xml =
                "<node><interface name=\"" IFACE "\">"
                "<method name=\"trigger_mount\"/>"
                "<method name=\"trigger_unmount\"/>"
                "<signal name=\"UsbMount\">"
                "<arg type=\"s\"/><arg type=\"i\"/><arg type=\"s\"/><arg type=\"s\"/>"
                "</signal>"
                "<signal name=\"UsbUnMount\">"
                "<arg type=\"s\"/><arg type=\"i\"/><arg type=\"s\"/><arg type=\"s\"/>"
                "</signal></interface></node>";
            DBusMessage *reply = dbus_message_new_method_return(request);
            if (reply != NULL &&
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
                                         DBUS_TYPE_INVALID))
                send_message(connection, reply);
            else if (reply != NULL)
                dbus_message_unref(reply);
        } else {
            send_message(connection, dbus_message_new_method_return(request));
        }
        dbus_message_unref(request);
    }

    dbus_bus_release_name(connection, BUS_NAME, NULL);
    dbus_connection_unref(connection);
    return 0;
}
