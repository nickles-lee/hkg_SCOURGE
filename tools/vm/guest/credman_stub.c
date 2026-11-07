/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Minimal system-bus stand-in for com.lge.car.credman.ServiceManager1.
 *
 * AppDMClientEngine calls GetData(name: ay) and expects the key blob as an
 * ay reply. The VM has the vendor teruten5_pub.key on disk but no .keystore /
 * TEE, so this stub logs the requested name and returns the configured key
 * file. Research infrastructure, not OEM code.
 *
 * usage: credman-stub [key-file]
 */
#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUS_NAME "com.lge.car.credman"
#define PATH     "/com/lge/car/credman"
#define IFACE    "com.lge.car.credman.ServiceManager1"

static volatile sig_atomic_t keep_running = 1;
static unsigned char key_data[65536];
static int key_len;
static char key_path[4096];

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static int load_key(const char *path)
{
    FILE *fh = fopen(path, "rb");
    size_t n;
    if (fh == NULL)
        return 0;
    n = fread(key_data, 1, sizeof(key_data), fh);
    fclose(fh);
    key_len = (int)n;
    return n > 0;
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

static DBusMessage *byte_array_reply(DBusMessage *request,
                                     const unsigned char *data, int len)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter root, array;
    if (reply == NULL)
        return NULL;
    dbus_message_iter_init_append(reply, &root);
    if (!dbus_message_iter_open_container(&root, DBUS_TYPE_ARRAY,
                                           DBUS_TYPE_BYTE_AS_STRING, &array) ||
        !dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE,
                                               &data, len) ||
        !dbus_message_iter_close_container(&root, &array)) {
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

static int read_name(DBusMessage *request, char *out, size_t out_len)
{
    DBusMessageIter iter, array;
    const unsigned char *data = NULL;
    int len = 0;
    size_t i;
    if (!dbus_message_iter_init(request, &iter))
        return 0;
    if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
        dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_BYTE)
        return 0;
    dbus_message_iter_recurse(&iter, &array);
    dbus_message_iter_get_fixed_array(&array, &data, &len);
    if (len <= 0 || (size_t)len >= out_len)
        return 0;
    for (i = 0; i < (size_t)len; ++i)
        out[i] = (char)data[i];
    out[len] = '\0';
    return 1;
}

int main(int argc, char **argv)
{
    DBusConnection *connection;
    DBusError error;
    int request_result;

    snprintf(key_path, sizeof(key_path), "%s",
             argc > 1 ? argv[1] : "/app/share/AppUpgrade/teruten5_pub.key");
    if (!load_key(key_path)) {
        fprintf(stderr, "credman-stub: cannot read key file %s\n", key_path);
        return 2;
    }

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection == NULL) {
        fprintf(stderr, "credman-stub: system bus: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return 3;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    request_result = dbus_bus_request_name(connection, BUS_NAME,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "credman-stub: request_name: %s: %s\n",
                error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 4;
    }
    if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "credman-stub: name busy rc=%d\n", request_result);
        dbus_connection_unref(connection);
        return 5;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    fprintf(stderr, "CREDMAN_STUB_READY key=%s bytes=%d\n", key_path, key_len);
    fflush(stderr);

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
        fprintf(stderr,
                "CREDMAN_STUB_CALL path=%s interface=%s member=%s signature=%s\n",
                dbus_message_get_path(request),
                dbus_message_get_interface(request) ? dbus_message_get_interface(request) : "(null)",
                member ? member : "(null)",
                dbus_message_get_signature(request));
        fflush(stderr);

        if (dbus_message_is_method_call(
                request, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            const char *xml =
                "<node><interface name=\"" IFACE "\">"
                "<method name=\"PutData\">"
                "<arg name=\"name\" type=\"ay\" direction=\"in\"/>"
                "<arg name=\"data\" type=\"ay\" direction=\"in\"/>"
                "</method>"
                "<method name=\"GetData\">"
                "<arg name=\"name\" type=\"ay\" direction=\"in\"/>"
                "<arg name=\"data\" type=\"ay\" direction=\"out\"/>"
                "</method></interface></node>";
            DBusMessage *reply = dbus_message_new_method_return(request);
            if (reply != NULL &&
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
                                         DBUS_TYPE_INVALID))
                send_message(connection, reply);
            else if (reply != NULL)
                dbus_message_unref(reply);
        } else if (member != NULL && strcmp(member, "GetData") == 0) {
            char name[512] = "(unreadable)";
            if (read_name(request, name, sizeof(name)))
                fprintf(stderr, "CREDMAN_STUB_GETDATA name=%s\n", name);
            else
                fprintf(stderr, "CREDMAN_STUB_GETDATA unreadable\n");
            fflush(stderr);
            send_message(connection, byte_array_reply(request, key_data, key_len));
        } else if (member != NULL && strcmp(member, "PutData") == 0) {
            send_message(connection, dbus_message_new_method_return(request));
        } else {
            send_message(connection, dbus_message_new_error(
                request, DBUS_ERROR_UNKNOWN_METHOD,
                "method not implemented by credman research stub"));
        }
        dbus_message_unref(request);
    }

    dbus_bus_release_name(connection, BUS_NAME, NULL);
    dbus_connection_unref(connection);
    return 0;
}
