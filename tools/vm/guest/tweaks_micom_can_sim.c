/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * tweaks_micom_can_sim - VM stand-in for the vendor micom CAN interface.
 *
 * Owns com.lge.car.micom on the system bus (the bus name the micom service
 * owns per its systemd unit; the CAN interface is com.lge.car.micom.can at
 * path /Can - destination com.lge.car.micom, interface com.lge.car.micom.can)
 * and logs every call with full arguments:
 *
 *   TWEAKS_CAN_SIM_READY
 *   TWEAKS_CAN_SIM_CALL sender=... path=... interface=... member=... signature=...
 *   TWEAKS_CAN_SIM_ARG index=0 type=ay length=2 hex=063f
 *   TWEAKS_CAN_SIM_ARG index=1 type=i value=26
 *   ...
 *   TWEAKS_CAN_SIM_ACK member=SetCANFixedMicomRecv
 *
 * Implements exactly the introspection the vendor publishes:
 *   SetCANFixedMicomRecv(ay canid, i startbit, i size, i data)
 *   SetCANMicomRecv(ay)
 * Anything else fails closed with UnknownMethod.
 */
#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUS_NAME "com.lge.car.micom"
#define CAN_PATH "/Can"
#define CAN_INTERFACE "com.lge.car.micom.can"

static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static void log_arguments(DBusMessage *message)
{
    DBusMessageIter iter;
    int index = 0;
    dbus_message_iter_init(message, &iter);
    for (;;) {
        int type = dbus_message_iter_get_arg_type(&iter);
        if (type == DBUS_TYPE_INVALID)
            break;
        if (type == DBUS_TYPE_ARRAY) {
            DBusMessageIter sub;
            const uint8_t *data;
            int len;
            int i;
            dbus_message_iter_recurse(&iter, &sub);
            dbus_message_iter_get_fixed_array(&sub, &data, &len);
            printf("TWEAKS_CAN_SIM_ARG index=%d type=ay length=%d hex=", index, len);
            for (i = 0; i < len; ++i)
                printf("%02x", data[i]);
            printf("\n");
        } else if (type == DBUS_TYPE_INT32) {
            dbus_int32_t value;
            dbus_message_iter_get_basic(&iter, &value);
            printf("TWEAKS_CAN_SIM_ARG index=%d type=i value=%d\n", index, value);
        } else {
            printf("TWEAKS_CAN_SIM_ARG index=%d type=%c unsupported\n", index, type);
        }
        dbus_message_iter_next(&iter);
        ++index;
    }
}

static void send_empty(DBusConnection *connection, DBusMessage *request)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (reply != NULL) {
        dbus_connection_send(connection, reply, NULL);
        dbus_connection_flush(connection);
        dbus_message_unref(reply);
    }
}

static void send_error(DBusConnection *connection, DBusMessage *request,
                       const char *name, const char *message)
{
    DBusMessage *reply = dbus_message_new_error(request, name, message);
    if (reply != NULL) {
        dbus_connection_send(connection, reply, NULL);
        dbus_connection_flush(connection);
        dbus_message_unref(reply);
    }
}

int main(void)
{
    static const char can_xml[] =
        "<node><interface name=\"" CAN_INTERFACE "\">"
        "<method name=\"SetCANFixedMicomRecv\">"
        "<arg direction=\"in\" type=\"ay\"/>"
        "<arg direction=\"in\" type=\"i\"/>"
        "<arg direction=\"in\" type=\"i\"/>"
        "<arg direction=\"in\" type=\"i\"/>"
        "</method>"
        "<method name=\"SetCANMicomRecv\">"
        "<arg direction=\"in\" type=\"ay\"/>"
        "</method>"
        "</interface>"
        "<interface name=\"org.freedesktop.DBus.Introspectable\">"
        "<method name=\"Introspect\"><arg direction=\"out\" type=\"s\"/>"
        "</method></interface></node>";
    DBusConnection *connection;
    DBusError error;
    int request_result;

    signal(SIGTERM, stop_handler);
    signal(SIGINT, stop_handler);

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (connection == NULL) {
        fprintf(stderr, "tweaks_micom_can_sim: dbus_bus_get: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return 3;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    request_result = dbus_bus_request_name(connection, BUS_NAME,
                                            DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "tweaks_micom_can_sim: name request: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 4;
    }
    if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "tweaks_micom_can_sim: name rejected: %d\n", request_result);
        dbus_connection_unref(connection);
        return 5;
    }
    printf("TWEAKS_CAN_SIM_READY\n");
    fflush(stdout);

    while (keep_running && dbus_connection_read_write_dispatch(connection, 500)) {
        DBusMessage *request;
        while ((request = dbus_connection_pop_message(connection)) != NULL) {
            const char *path = dbus_message_get_path(request);
            const char *member = dbus_message_get_member(request);
            const char *signature = dbus_message_get_signature(request);
            int handled = 0;

            if (dbus_message_get_type(request) == DBUS_MESSAGE_TYPE_METHOD_CALL) {
                printf("TWEAKS_CAN_SIM_CALL sender=%s path=%s interface=%s "
                       "member=%s signature=%s\n",
                       dbus_message_get_sender(request) ? dbus_message_get_sender(request) : "(null)",
                       path ? path : "(null)",
                       dbus_message_get_interface(request) ? dbus_message_get_interface(request) : "(null)",
                       member ? member : "(null)",
                       signature ? signature : "(null)");
                if (path != NULL && strcmp(path, CAN_PATH) == 0 &&
                    dbus_message_is_method_call(request, "org.freedesktop.DBus.Introspectable",
                                                "Introspect") &&
                    strcmp(signature, "") == 0) {
                    send_error(connection, request, DBUS_ERROR_FAILED, "introspect unused");
                    handled = 1;
                } else if (path != NULL && strcmp(path, CAN_PATH) == 0 &&
                           dbus_message_is_method_call(request, CAN_INTERFACE, "SetCANFixedMicomRecv") &&
                           strcmp(signature, "ayiii") == 0) {
                    log_arguments(request);
                    printf("TWEAKS_CAN_SIM_ACK member=SetCANFixedMicomRecv\n");
                    fflush(stdout);
                    send_empty(connection, request);
                    handled = 1;
                } else if (path != NULL && strcmp(path, CAN_PATH) == 0 &&
                           dbus_message_is_method_call(request, CAN_INTERFACE, "SetCANMicomRecv") &&
                           strcmp(signature, "ay") == 0) {
                    log_arguments(request);
                    printf("TWEAKS_CAN_SIM_ACK member=SetCANMicomRecv\n");
                    fflush(stdout);
                    send_empty(connection, request);
                    handled = 1;
                }
                if (!handled)
                    send_error(connection, request, DBUS_ERROR_UNKNOWN_METHOD,
                               "method not implemented by tweaks micom can sim");
            }
            dbus_message_unref(request);
        }
        fflush(stdout);
    }

    dbus_bus_release_name(connection, BUS_NAME, NULL);
    dbus_connection_unref(connection);
    (void)can_xml;
    return 0;
}
