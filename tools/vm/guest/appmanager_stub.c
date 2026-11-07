/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Narrow session-bus stand-in for the Gen5W ApplicationManager information
 * interface. It exists only to supply the vehicle/app-manager state that a
 * real head unit would provide: in particular ask_standby_status() = 12,
 * which is the state AppDMClientEngine requires before it processes a USB
 * update. Every call is logged. Research infrastructure, not OEM firmware.
 *
 * usage: appmanager-stub [standby-status]
 */
#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVICE "com.lge.hyundai.avn.appmanager"
#define IFACE   "com.lge.avn.appmanager.info"

static volatile sig_atomic_t keep_running = 1;

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static const char introspection_xml[] =
    "<node>"
    "<interface name=\"" IFACE "\">"
    "<method name=\"ask_standby_status\">"
    "<arg direction=\"out\" type=\"i\" name=\"status\"/></method>"
    "<method name=\"ask_power_status\">"
    "<arg direction=\"out\" type=\"i\" name=\"status\"/></method>"
    "<method name=\"trigger_standby\"/>"
    "<signal name=\"standby_status\">"
    "<arg type=\"i\" name=\"status\"/></signal>"
    "<signal name=\"sleep_status\">"
    "<arg type=\"i\" name=\"status\"/></signal>"
    "</interface>"
    "<interface name=\"org.freedesktop.DBus.Introspectable\">"
    "<method name=\"Introspect\">"
    "<arg direction=\"out\" type=\"s\"/></method>"
    "</interface>"
    "</node>";

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

static DBusMessage *int_reply(DBusMessage *request, int32_t value)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (reply == NULL)
        return NULL;
    if (!dbus_message_append_args(reply, DBUS_TYPE_INT32, &value,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

static DBusMessage *string_reply(DBusMessage *request, const char *value)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    if (reply == NULL)
        return NULL;
    if (!dbus_message_append_args(reply, DBUS_TYPE_STRING, &value,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(reply);
        return NULL;
    }
    return reply;
}

static void emit_status(DBusConnection *connection, const char *path,
                        const char *signal, int32_t value)
{
    DBusMessage *msg = dbus_message_new_signal(path, IFACE, signal);
    if (msg == NULL)
        return;
    if (!dbus_message_append_args(msg, DBUS_TYPE_INT32, &value,
                                  DBUS_TYPE_INVALID)) {
        dbus_message_unref(msg);
        return;
    }
    if (dbus_connection_send(connection, msg, NULL))
        dbus_connection_flush(connection);
    dbus_message_unref(msg);
}

int main(int argc, char **argv)
{
    DBusConnection *connection;
    DBusError error;
    int32_t standby_status = 12;
    int request_result;

    if (argc > 1)
        standby_status = (int32_t)strtol(argv[1], NULL, 0);

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (connection == NULL) {
        fprintf(stderr, "appmanager-stub: session bus: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return 2;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    request_result = dbus_bus_request_name(connection, SERVICE,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "appmanager-stub: request_name: %s: %s\n",
                error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 3;
    }
    if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "appmanager-stub: name busy rc=%d\n", request_result);
        dbus_connection_unref(connection);
        return 4;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    fprintf(stderr, "APPMANAGER_STUB_READY service=%s standby=%d\n",
            SERVICE, standby_status);
    fflush(stderr);

    while (keep_running && dbus_connection_get_is_connected(connection)) {
        DBusMessage *request;
        const char *path;
        const char *iface;
        const char *member;

        dbus_connection_read_write(connection, 500);
        request = dbus_connection_pop_message(connection);
        if (request == NULL)
            continue;
        if (dbus_message_get_type(request) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
            dbus_message_unref(request);
            continue;
        }
        path = dbus_message_get_path(request);
        iface = dbus_message_get_interface(request);
        member = dbus_message_get_member(request);
        fprintf(stderr,
                "APPMANAGER_STUB_CALL path=%s interface=%s member=%s signature=%s\n",
                path ? path : "(null)", iface ? iface : "(null)",
                member ? member : "(null)",
                dbus_message_get_signature(request));
        fflush(stderr);

        if (dbus_message_is_method_call(
                request, "org.freedesktop.DBus.Introspectable", "Introspect")) {
            send_message(connection, string_reply(request, introspection_xml));
        } else if (dbus_message_is_method_call(
                       request, "org.freedesktop.DBus.Peer", "Ping")) {
            send_message(connection, dbus_message_new_method_return(request));
        } else if (member != NULL && strcmp(member, "ask_standby_status") == 0) {
            send_message(connection, int_reply(request, standby_status));
        } else if (member != NULL && strcmp(member, "ask_power_status") == 0) {
            send_message(connection, int_reply(request, 1));
        } else if (member != NULL && strcmp(member, "ask_acc_status") == 0) {
            send_message(connection, int_reply(request, 1));
        } else if (member != NULL && strcmp(member, "ask_ign_status") == 0) {
            send_message(connection, int_reply(request, 1));
        } else if (member != NULL && strcmp(member, "ask_valet_mode_status") == 0) {
            /* Explicit research fixture: valet mode disabled. The client does
             * not tolerate an error reply here; unknown ask_* calls still fail. */
            send_message(connection, int_reply(request, 0));
        } else if (member != NULL && strcmp(member, "registration") == 0) {
            send_message(connection, dbus_message_new_method_return(request));
        } else if (member != NULL && strcmp(member, "trigger_standby") == 0) {
            standby_status = 12;
            emit_status(connection, "/", "standby_status", standby_status);
            emit_status(connection, "/Information", "standby_status", standby_status);
            fprintf(stderr, "APPMANAGER_STUB_STANDBY_EMITTED %d\n", standby_status);
            fflush(stderr);
            send_message(connection, dbus_message_new_method_return(request));
        } else {
            send_message(connection, dbus_message_new_error(
                request, DBUS_ERROR_UNKNOWN_METHOD,
                "method not implemented by AppManager research stub"));
        }
        dbus_message_unref(request);
    }

    dbus_bus_release_name(connection, SERVICE, NULL);
    dbus_connection_unref(connection);
    return 0;
}
