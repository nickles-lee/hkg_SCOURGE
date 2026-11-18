/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline cp_spp_dbus tests against a real private dbus-daemon and a
 * first-party mock OEM service.  No host system bus, no service activation,
 * no OEM binary, no real SPP data and no MFi/chip authentication are used.
 *
 * The child process in this file is the mock service.  The parent is the
 * engine under test.  Every scenario gets a fresh private bus from the Python
 * harness; the child requests only the documented adapter name.
 */
#define _GNU_SOURCE
#include "../src/carplay/cp_spp_dbus.h"

#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Observe close's explicit I/O, not libdbus's implementation-dependent
 * immediate submission of a method before close was requested. */
static bool watch_close_io;
static unsigned close_io_calls;
extern dbus_bool_t __real_dbus_connection_read_write(DBusConnection *, int);
dbus_bool_t __wrap_dbus_connection_read_write(DBusConnection *connection, int ms)
{
    if (watch_close_io) close_io_calls++;
    return __real_dbus_connection_read_write(connection, ms);
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define ADAPTER_SERVICE CP_SPP_DBUS_ADAPTER_SERVICE
#define ADAPTER_PATH CP_SPP_DBUS_ADAPTER_PATH
#define ADAPTER_IFACE CP_SPP_DBUS_ADAPTER_INTERFACE
#define SERIAL_IFACE CP_SPP_DBUS_SERIAL_INTERFACE
#define SERIAL_PATH "/dev_AA_BB_CC_DD_EE_FF/serial_00000000_DECA_FADE_DECA_DEAFDECACAFF"
#define SERIAL_SERVICE CP_SPP_DBUS_SERIAL_SERVICE_PREFIX "dev_AA_BB_CC_DD_EE_FF.serial_00000000_DECA_FADE_DECA_DEAFDECACAFF"
#define TEST_SERVICE "SCOURGE Test SPP"
#define TEST_PEER "AA:BB:CC:DD:EE:FF"

static const char *k_adapter_xml_two =
    "<?xml version=\"1.0\"?>"
    "<node>"
    "<interface name=\"" ADAPTER_IFACE "\">"
    "<method name=\"ListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"StopListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "</interface>"
    "</node>";

static const char *k_adapter_xml_one =
    "<?xml version=\"1.0\"?>"
    "<node>"
    "<interface name=\"" ADAPTER_IFACE "\">"
    "<method name=\"ListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"StopListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "</interface>"
    "</node>";

static const char *k_adapter_xml_bad =
    "<?xml version=\"1.0\"?>"
    "<node>"
    "<interface name=\"" ADAPTER_IFACE "\">"
    "<method name=\"ListenSPP\">"
    "<arg type=\"i\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"StopListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "</interface>"
    "</node>";

static const char *k_adapter_xml_dtd =
    "<?xml version=\"1.0\"?>"
    "<!DOCTYPE node SYSTEM \"file:///etc/passwd\">"
    "<node>"
    "<interface name=\"" ADAPTER_IFACE "\">"
    "<method name=\"ListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"StopListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "</interface>"
    "</node>";

static const char *k_serial_xml =
    "<?xml version=\"1.0\"?>"
    "<node>"
    "<interface name=\"" SERIAL_IFACE "\">"
    "<method name=\"SendData\">"
    "<arg type=\"ay\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"Disconnect\">"
    "<arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<signal name=\"onStatusAttributeChanged\">"
    "<arg type=\"s\" direction=\"out\"/>"
    "<arg type=\"s\" direction=\"out\"/>"
    "</signal>"
    "</interface>"
    "</node>";

typedef struct {
    DBusConnection *conn;
    int ready_fd;
    int cmd_fd;
    const char *scenario;
    bool listener;
    bool emitted;
    bool no_listen_reply;
    bool no_send_reply;
    bool stop_seen;
    int listen_reply;
    int send_reply;
    int send_count;
    size_t last_len;
    uint8_t last[CP_SPP_DBUS_MAX_TX_BYTES];
} mock_t;

static void mock_reply_int(DBusConnection *conn, DBusMessage *call, int value)
{
    DBusMessage *reply = dbus_message_new_method_return(call);
    DBusMessageIter iter;
    dbus_int32_t v = (dbus_int32_t)value;
    if (reply == NULL) return;
    dbus_message_iter_init_append(reply, &iter);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_INT32, &v);
    (void)dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
}

static void mock_reply_string(DBusConnection *conn, DBusMessage *call,
                              const char *value)
{
    DBusMessage *reply = dbus_message_new_method_return(call);
    DBusMessageIter iter;
    if (reply == NULL) return;
    dbus_message_iter_init_append(reply, &iter);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value);
    (void)dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
}

static void mock_reply_error(DBusConnection *conn, DBusMessage *call,
                             const char *name)
{
    DBusMessage *reply = dbus_message_new_error(call, name, "mock error");
    if (reply == NULL) return;
    (void)dbus_connection_send(conn, reply, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(reply);
}

static DBusHandlerResult mock_filter(DBusConnection *conn, DBusMessage *msg,
                                     void *data)
{
    mock_t *mock = data;
    if (dbus_message_get_type(msg) != DBUS_MESSAGE_TYPE_METHOD_CALL)
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    const char *path = dbus_message_get_path(msg);
    if (dbus_message_is_method_call(msg, "org.freedesktop.DBus.Introspectable",
                                    "Introspect")) {
        const char *xml = NULL;
        if (path != NULL && strcmp(path, ADAPTER_PATH) == 0) {
            if (strcmp(mock->scenario, "bad_signature") == 0)
                xml = k_adapter_xml_bad;
            else if (strcmp(mock->scenario, "xml_dtd") == 0)
                xml = k_adapter_xml_dtd;
            else if (strcmp(mock->scenario, "one_arg") == 0)
                xml = k_adapter_xml_one;
            else
                xml = k_adapter_xml_two;
        } else if (path != NULL && strcmp(path, SERIAL_PATH) == 0) {
            xml = k_serial_xml;
        }
        if (xml == NULL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        mock_reply_string(conn, msg, xml);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, ADAPTER_PATH) == 0 &&
        dbus_message_is_method_call(msg, ADAPTER_IFACE, "ListenSPP")) {
        const char *signature = dbus_message_get_signature(msg);
        bool ok = false;
        if (signature != NULL && strcmp(signature, "ss") == 0) {
            const char *uuid = NULL, *name = NULL;
            ok = dbus_message_get_args(msg, NULL,
                DBUS_TYPE_STRING, &uuid, DBUS_TYPE_STRING, &name,
                DBUS_TYPE_INVALID) &&
                uuid != NULL && name != NULL &&
                strcmp(uuid, CP_SPP_DBUS_UUID_CAFF) == 0 &&
                strcmp(name, TEST_SERVICE) == 0;
        } else if (signature != NULL && strcmp(signature, "s") == 0) {
            const char *name = NULL;
            ok = dbus_message_get_args(msg, NULL,
                DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) &&
                name != NULL && strcmp(name, TEST_SERVICE) == 0;
        }
        if (!ok) {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        mock->listener = true;
        if (!mock->no_listen_reply)
            mock_reply_int(conn, msg, mock->listen_reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, ADAPTER_PATH) == 0 &&
        dbus_message_is_method_call(msg, ADAPTER_IFACE, "StopListenSPP")) {
        const char *uuid = NULL, *name = NULL;
        if (dbus_message_get_signature(msg) != NULL &&
            strcmp(dbus_message_get_signature(msg), "ss") == 0 &&
            dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &uuid,
                DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID) &&
            !strcmp(uuid, CP_SPP_DBUS_UUID_CAFF) && !strcmp(name, TEST_SERVICE)) {
            mock->stop_seen = true;
            mock->listener = false;
            mock_reply_int(conn, msg, 0);
        } else {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, SERIAL_PATH) == 0 &&
        dbus_message_is_method_call(msg, SERIAL_IFACE, "SendData")) {
        DBusMessageIter iter, array;
        int count = 0;
        size_t used = 0;
        if (!dbus_message_iter_init(msg, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_BYTE) {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_recurse(&iter, &array);
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE) {
            unsigned char byte = 0;
            dbus_message_iter_get_basic(&array, &byte);
            if (used < sizeof(mock->last)) mock->last[used] = byte;
            used++;
            count++;
            if (count > (int)CP_SPP_DBUS_MAX_TX_BYTES) break;
            dbus_message_iter_next(&array);
        }
        mock->send_count++;
        mock->last_len = used;
        if (!mock->no_send_reply) mock_reply_int(conn, msg, mock->send_reply);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, SERIAL_PATH) == 0 &&
        dbus_message_is_method_call(msg, SERIAL_IFACE, "Disconnect")) {
        mock_reply_int(conn, msg, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void mock_emit_status(DBusConnection *conn, const char *status)
{
    DBusMessage *message = dbus_message_new_signal(
        SERIAL_PATH, SERIAL_IFACE, CP_SPP_DBUS_SERIAL_SIGNAL_STATUS);
    DBusMessageIter iter;
    const char *path = SERIAL_PATH;
    if (message == NULL) return;
    dbus_message_iter_init_append(message, &iter);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &path);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &status);
    (void)dbus_connection_send(conn, message, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(message);
}

static void mock_bad_status(DBusConnection *conn, bool path_bad)
{
    const char *path = path_bad ? "/serial0" : SERIAL_PATH;
    const char *status = "connected";
    DBusMessage *message = dbus_message_new_signal(
        path, SERIAL_IFACE, CP_SPP_DBUS_SERIAL_SIGNAL_STATUS);
    CHECK(message != NULL);
    if (path_bad)
        CHECK(dbus_message_append_args(message, DBUS_TYPE_STRING, &path,
            DBUS_TYPE_STRING, &status, DBUS_TYPE_INVALID));
    else
        CHECK(dbus_message_append_args(message, DBUS_TYPE_STRING, &status,
                                       DBUS_TYPE_INVALID));
    CHECK(dbus_connection_send(conn, message, NULL));
    dbus_connection_flush(conn);
    dbus_message_unref(message);
}

static void mock_impostor_status(const char *address, const char *status)
{
    DBusError error;
    DBusConnection *conn;
    dbus_error_init(&error);
    conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) {
        dbus_error_free(&error);
        return;
    }
    if (!dbus_bus_register(conn, &error)) {
        dbus_error_free(&error);
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return;
    }
    mock_emit_status(conn, status);
    dbus_connection_flush(conn);
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    dbus_error_free(&error);
}

static void mock_release_name(DBusConnection *conn)
{
    DBusError error;
    dbus_error_init(&error);
    (void)dbus_bus_release_name(conn, ADAPTER_SERVICE, &error);
    dbus_error_free(&error);
}

static void child_main(const char *address, const char *scenario,
                       int ready_fd, int cmd_fd)
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) _exit(2);
    if (!dbus_bus_register(conn, &error)) _exit(3);
    int flags = fcntl(cmd_fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(cmd_fd, F_SETFL, flags | O_NONBLOCK);
    mock_t mock = {0};
    mock.conn = conn;
    mock.scenario = scenario;
    mock.listen_reply = strcmp(scenario, "listen3") == 0 ? 3 : 0;
    mock.send_reply = strcmp(scenario, "send_error") == 0 ? 9 : 0;
    mock.no_listen_reply = strcmp(scenario, "listen_timeout") == 0;
    mock.no_send_reply = strcmp(scenario, "send_timeout") == 0;
    if (!dbus_connection_add_filter(conn, mock_filter, &mock, NULL)) _exit(4);
    int request = dbus_bus_request_name(conn, ADAPTER_SERVICE,
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(5);
    if (strcmp(scenario, "serial_missing")) {
        request = dbus_bus_request_name(conn, SERIAL_SERVICE,
            DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
        if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(7);
    }
    dbus_error_free(&error);
    char ready = 'R';
    (void)write(ready_fd, &ready, 1);
    close(ready_fd);
    for (;;) {
        (void)dbus_connection_read_write_dispatch(conn, 20);
        char command = 0;
        if (read(cmd_fd, &command, 1) == 1) {
            if (command == 'O') mock_release_name(conn);
            if (command == 'S') {
                dbus_error_init(&error);
                (void)dbus_bus_release_name(conn, SERIAL_SERVICE, &error);
                dbus_error_free(&error);
            }
            if (command == 'I') mock_impostor_status(address, "disconnected");
            if (command == 'D') mock_emit_status(conn, "disconnected");
            if (command == 'C') {
                for (unsigned i = 0; i < 10; i++)
                    (void)dbus_connection_read_write_dispatch(conn, 20);
                _exit(mock.send_count <= 1 ? 0 : 8);
            }
        }
        if (mock.listener && !mock.emitted && !mock.no_listen_reply) {
            if (strcmp(scenario, "wrong_case") == 0) {
                mock_emit_status(conn, "Connected");
                usleep(120000);
            }
            if (strcmp(scenario, "no_connect") != 0) {
                if (!strcmp(scenario, "bad_serial_path"))
                    mock_bad_status(conn, true);
                else if (!strcmp(scenario, "bad_status_signature"))
                    mock_bad_status(conn, false);
                else mock_emit_status(conn, "connected");
                mock.emitted = true;
            }
        }
    }
}

typedef struct {
    unsigned state_calls;
    unsigned validated;
    unsigned status_calls;
    unsigned removed;
    unsigned fatal_calls;
    unsigned rx_calls;
    unsigned ack_calls;
    size_t ack_len;
    unsigned last_rx_kind;
    size_t last_rx_len;
    char last_status[CP_SPP_DBUS_MAX_VALUE];
    char last_removed[256];
    char fatal_reason[CP_SPP_DBUS_MAX_VALUE];
    cp_spp_dbus_state last_state;
} capture_t;

static void cb_state(void *user, cp_spp_dbus_state old_state,
                     cp_spp_dbus_state new_state)
{
    (void)old_state;
    capture_t *cap = user;
    cap->state_calls++;
    cap->last_state = new_state;
}

static void cb_validated(void *user)
{
    ((capture_t *)user)->validated++;
}

static void cb_status(void *user, const char *object_path,
                      const char *status)
{
    capture_t *cap = user;
    (void)object_path;
    cap->status_calls++;
    snprintf(cap->last_status, sizeof(cap->last_status), "%s", status);
}

static void cb_removed(void *user, const char *object_path)
{
    capture_t *cap = user;
    cap->removed++;
    snprintf(cap->last_removed, sizeof(cap->last_removed), "%s", object_path);
}

static void cb_fatal(void *user, const char *reason)
{
    capture_t *cap = user;
    cap->fatal_calls++;
    snprintf(cap->fatal_reason, sizeof(cap->fatal_reason), "%s", reason);
}

static void cb_rx(void *user, const char *peer, uint8_t uuid_kind,
                  const uint8_t *data, size_t length)
{
    capture_t *cap = user;
    (void)peer;
    (void)data;
    cap->rx_calls++;
    cap->last_rx_kind = uuid_kind;
    cap->last_rx_len = length;
}

static void cb_ack(void *user, size_t length)
{
    capture_t *cap = user;
    cap->ack_calls++;
    cap->ack_len = length;
}

static cp_spp_dbus_config test_config(const char *address)
{
    cp_spp_dbus_config config;
    memset(&config, 0, sizeof(config));
    config.bus_address = address;
    config.service_name = TEST_SERVICE;
    config.expected_owner_uid_set = 1;
    config.expected_owner_uid = (uint32_t)geteuid();
    config.control_timeout_ms = 2000;
    config.send_timeout_ms = 1000;
    config.max_tx_bytes = CP_SPP_DBUS_MAX_TX_BYTES;
    config.experimental = 1;
    return config;
}

static uint64_t fixture_now_ms(void)
{
    struct timespec ts;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void tick_once(cp_spp_dbus *engine, uint64_t *now, int timeout_ms)
{
    struct pollfd pfd;
    if (cp_spp_dbus_pollfd(engine, &pfd) > 0) {
        int wait = cp_spp_dbus_timeout_ms(engine, *now);
        if (wait < 0 || wait > timeout_ms) wait = timeout_ms;
        (void)poll(&pfd, 1, wait > 0 ? wait : 0);
    }
    if (fixture_now_ms() == *now) usleep(1000);
    *now = fixture_now_ms();
    (void)cp_spp_dbus_tick(engine, *now);
}

static void wait_validated(cp_spp_dbus *engine, uint64_t *now,
                           capture_t *cap, unsigned limit)
{
    for (unsigned i = 0; i < limit && cap->validated == 0 &&
         cap->fatal_calls == 0; i++)
        tick_once(engine, now, 10);
}

static void wait_state(cp_spp_dbus *engine, uint64_t *now,
                       cp_spp_dbus_state wanted, capture_t *cap,
                       unsigned limit)
{
    for (unsigned i = 0; i < limit &&
         cp_spp_dbus_state_get(engine) != wanted &&
         cap->fatal_calls == 0; i++)
        tick_once(engine, now, 10);
}

static void wait_acks(cp_spp_dbus *engine, uint64_t *now, capture_t *cap,
                      unsigned wanted, unsigned limit)
{
    for (unsigned i = 0; i < limit && cap->ack_calls < wanted &&
         cap->fatal_calls == 0; i++)
        tick_once(engine, now, 10);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const char *address = argv[1];
    const char *scenario = argv[2];
    CHECK(address != NULL && scenario != NULL);

    /* Reject unsafe/no-op configurations before opening anything. */
    cp_spp_dbus_storage storage;
    cp_spp_dbus *engine = NULL;
    char error[256] = "";
    cp_spp_dbus_callbacks callbacks = {0};
    cp_spp_dbus_config invalid = test_config(address);
    invalid.experimental = 0;
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);
    invalid = test_config("autolaunch:");
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);
    invalid = test_config("tcp:host=127.0.0.1,port=1");
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);
    invalid = test_config(address);
    invalid.service_name = "";
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);
    invalid = test_config(address);
    invalid.max_tx_bytes = CP_SPP_DBUS_MAX_TX_BYTES + 1u;
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);
    invalid = test_config(address);
    invalid.uuid_phone = CP_SPP_DBUS_UUID_CAFF;
    CHECK(cp_spp_dbus_open(&engine, &storage, &invalid, &callbacks, NULL,
                           error, sizeof(error)) == CP_SPP_DBUS_ERR_INVALID);

    int ready_pipe[2], cmd_pipe[2];
    CHECK(pipe2(ready_pipe, O_CLOEXEC) == 0);
    CHECK(pipe2(cmd_pipe, O_CLOEXEC) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        close(cmd_pipe[1]);
        child_main(address, scenario, ready_pipe[1], cmd_pipe[0]);
        _exit(6);
    }
    close(ready_pipe[1]);
    close(cmd_pipe[0]);
    char ready = 0;
    CHECK(read(ready_pipe[0], &ready, 1) == 1 && ready == 'R');
    close(ready_pipe[0]);

    capture_t cap;
    memset(&cap, 0, sizeof(cap));
    callbacks.on_state = cb_state;
    callbacks.on_validated = cb_validated;
    callbacks.on_status = cb_status;
    callbacks.on_removed = cb_removed;
    callbacks.on_fatal = cb_fatal;
    callbacks.on_rx = cb_rx;
    callbacks.on_stub_ack = cb_ack;
    cp_spp_dbus_config config = test_config(address);
    if (!strcmp(scenario, "wrong_uid")) config.expected_owner_uid ^= 1u;
    CHECK(cp_spp_dbus_open(&engine, &storage, &config, &callbacks, &cap,
                           error, sizeof(error)) == CP_SPP_DBUS_OK);
    CHECK(engine != NULL);
    CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_OPEN);
    CHECK(cp_spp_dbus_adapter_validated(engine) == 0);
    CHECK(cp_spp_dbus_send(engine, (const uint8_t *)"x", 1, 0) ==
          CP_SPP_DBUS_ERR_NOT_CONNECTED);
    uint64_t now = fixture_now_ms();
    CHECK(cp_spp_dbus_start(engine, now) == CP_SPP_DBUS_OK);
    wait_validated(engine, &now, &cap, 4000);
    if (strcmp(scenario, "bad_signature") == 0 ||
        strcmp(scenario, "one_arg") == 0 ||
        strcmp(scenario, "wrong_uid") == 0 ||
        strcmp(scenario, "xml_dtd") == 0) {
        CHECK(cap.validated == 0 && cap.fatal_calls == 1);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus rejects unsafe/bad method introspection");
        return 0;
    }
    if (cap.validated != 1)
        fprintf(stderr, "DEBUG validated=%u fatal=%u reason=%s err=%s state=%d\n",
                cap.validated, cap.fatal_calls, cap.fatal_reason,
                cp_spp_dbus_error(engine), (int)cp_spp_dbus_state_get(engine));
    CHECK(cap.validated == 1);
    CHECK(cp_spp_dbus_adapter_validated(engine) == 1);
    CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_LISTENING);
    if (strcmp(scenario, "listen_timeout") == 0) {
        CHECK(cp_spp_dbus_listen(engine, now) == CP_SPP_DBUS_OK);
        now += 3000;
        (void)cp_spp_dbus_tick(engine, now);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        CHECK(cap.fatal_calls == 1);
        CHECK(cp_spp_dbus_listener_owned(engine) == 0);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus bounds ListenSPP and never assumes ownership");
        return 0;
    }
    CHECK(cp_spp_dbus_listen(engine, now) == CP_SPP_DBUS_OK);
    for (unsigned i = 0; i < 2000 && cp_spp_dbus_listener_owned(engine) == 0 &&
         cap.fatal_calls == 0; i++)
        tick_once(engine, &now, 10);
    if (strcmp(scenario, "listen3") == 0) {
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        CHECK(cp_spp_dbus_listener_owned(engine) == 0);
        CHECK(strstr(cap.fatal_reason, "listen_rejected_3") != NULL);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus rejects error 3 without foreign listener ownership");
        return 0;
    }
    CHECK(cp_spp_dbus_listener_owned(engine) == 1);
    if (!strcmp(scenario, "serial_missing") ||
        !strcmp(scenario, "bad_serial_path") ||
        !strcmp(scenario, "bad_status_signature")) {
        wait_state(engine, &now, CP_SPP_DBUS_STATE_FATAL, &cap, 4000);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        CHECK(cap.fatal_calls == 1 && cap.rx_calls == 0);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]); kill(child, SIGTERM); (void)waitpid(child, NULL, 0);
        puts("PASS: unbound object/name/status cannot become connected");
        return 0;
    }
    if (strcmp(scenario, "no_connect") == 0) {
        for (unsigned i = 0; i < 50; i++) tick_once(engine, &now, 10);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_LISTENING);
        CHECK(cp_spp_dbus_send(engine, (const uint8_t *)"x", 1, now) ==
              CP_SPP_DBUS_ERR_NOT_CONNECTED);
        CHECK(cp_spp_dbus_deliver_rx(engine, TEST_PEER,
                                     CP_SPP_DBUS_RX_UUID_KIND_CAFF,
                                     (const uint8_t *)"x", 1) ==
              CP_SPP_DBUS_ERR_NOT_CONNECTED);
        CHECK(cp_spp_dbus_stop(engine, now) == CP_SPP_DBUS_OK);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus does not fabricate a connected transport");
        return 0;
    }
    if (strcmp(scenario, "wrong_case") == 0) {
        for (unsigned i = 0; i < 200 && cap.status_calls == 0; i++)
            tick_once(engine, &now, 10);
        CHECK(cap.status_calls >= 1);
        CHECK(strcmp(cap.last_status, "Connected") == 0);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_LISTENING);
    }
    wait_state(engine, &now, CP_SPP_DBUS_STATE_CONNECTED, &cap, 4000);
    CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_CONNECTED);
    CHECK(cap.status_calls >= 1);
    /* A status transition is not a trusted receiver observation. */
    CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_CONNECTED);
    if (!strcmp(scenario, "close_inflight") || !strcmp(scenario, "close_fatal")) {
        uint8_t byte = 0x55;
        CHECK(cp_spp_dbus_send(engine, &byte, 1, now) == CP_SPP_DBUS_OK);
        CHECK(cp_spp_dbus_send(engine, &byte, 1, now) == CP_SPP_DBUS_OK);
        /* Only the first may be submitted; the second remains application
         * queued. An already-submitted method cannot be recalled. */
        CHECK(cp_spp_dbus_tick(engine, now) == CP_SPP_DBUS_OK);
        CHECK(cp_spp_dbus_tx_queued(engine) == 2);
        if (!strcmp(scenario, "close_fatal")) {
            CHECK(cp_spp_dbus_tick(engine, now + 1001) == CP_SPP_DBUS_ERR_STATE);
            CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        }
        watch_close_io = true;
        cp_spp_dbus_close(engine);
        watch_close_io = false;
        CHECK(close_io_calls == 0);
        char command = 'C';
        CHECK(write(cmd_pipe[1], &command, 1) == 1);
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        close(cmd_pipe[1]);
        puts("PASS: outstanding/expired close performs no I/O or new frame submission");
        return 0;
    }
    if (strcmp(scenario, "owner") == 0 || !strcmp(scenario, "serial_owner")) {
        char command = !strcmp(scenario, "serial_owner") ? 'S' : 'O';
        CHECK(write(cmd_pipe[1], &command, 1) == 1);
        for (unsigned i = 0; i < 2000 && cap.fatal_calls == 0; i++)
            tick_once(engine, &now, 10);
        CHECK(cap.fatal_calls == 1);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        CHECK(strstr(cap.fatal_reason, "owner_lost") != NULL);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus makes adapter owner loss fatal");
        return 0;
    }
    if (strcmp(scenario, "impostor") == 0) {
        char command = 'I';
        CHECK(write(cmd_pipe[1], &command, 1) == 1);
        for (unsigned i = 0; i < 2000 && cap.fatal_calls == 0; i++)
            tick_once(engine, &now, 10);
        CHECK(cap.fatal_calls == 1);
        CHECK(strstr(cap.fatal_reason, "status_unexpected_owner") != NULL);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]);
        kill(child, SIGTERM);
        (void)waitpid(child, NULL, 0);
        puts("PASS: cp_spp_dbus rejects status from an unexpected owner");
        return 0;
    }

    uint8_t frame[CP_SPP_DBUS_MAX_TX_BYTES];
    for (size_t i = 0; i < sizeof(frame); i++) frame[i] = (uint8_t)(i * 7u);
    CHECK(cp_spp_dbus_send(engine, frame, sizeof(frame), now) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_send(engine, frame, sizeof(frame), now) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_send(engine, frame, sizeof(frame), now) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_send(engine, frame, sizeof(frame), now) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_send(engine, frame, sizeof(frame), now) ==
          CP_SPP_DBUS_ERR_BACKPRESSURE);
    CHECK(cp_spp_dbus_send(engine, frame, CP_SPP_DBUS_MAX_TX_BYTES + 1u, now) ==
          CP_SPP_DBUS_ERR_INVALID);
    CHECK(cp_spp_dbus_send(engine, frame, 0, now) == CP_SPP_DBUS_ERR_INVALID);
    if (!strcmp(scenario, "send_error") || !strcmp(scenario, "send_timeout") ||
        !strcmp(scenario, "disconnect")) {
        if (!strcmp(scenario, "disconnect")) {
            char command = 'D';
            CHECK(write(cmd_pipe[1], &command, 1) == 1);
        }
        wait_state(engine, &now, CP_SPP_DBUS_STATE_FATAL, &cap, 4000);
        CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_FATAL);
        CHECK(cp_spp_dbus_tx_queued(engine) == 0);
        CHECK(cap.fatal_calls == 1);
        CHECK(cp_spp_dbus_deliver_rx(engine, TEST_PEER,
            CP_SPP_DBUS_RX_UUID_KIND_CAFF, (const uint8_t *)"rx", 2) ==
            CP_SPP_DBUS_ERR_NOT_CONNECTED);
        cp_spp_dbus_close(engine);
        close(cmd_pipe[1]); kill(child, SIGTERM); (void)waitpid(child, NULL, 0);
        puts("PASS: send failure/timeout/disconnect retires queued transport bytes");
        return 0;
    }
    wait_acks(engine, &now, &cap, 4, 8000);
    CHECK(cap.ack_calls == 4);
    CHECK(cap.ack_len == sizeof(frame));
    CHECK(cp_spp_dbus_tx_queued(engine) == 0);

    char peer[18];
    CHECK(cp_spp_dbus_peer(engine, peer, sizeof(peer)) == CP_SPP_DBUS_OK);
    CHECK(!strcmp(peer, TEST_PEER));
    CHECK(cp_spp_dbus_deliver_rx(engine, "11:22:33:44:55:66",
          CP_SPP_DBUS_RX_UUID_KIND_CAFF, (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_ERR_REJECTED); /* even the FIRST datagram cannot rebind */

    CHECK(cp_spp_dbus_deliver_rx(engine, TEST_PEER,
                                 CP_SPP_DBUS_RX_UUID_KIND_CAFF,
                                 (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_deliver_rx(engine, "aa:bb:cc:dd:ee:ff",
                                 CP_SPP_DBUS_RX_UUID_KIND_CAFF,
                                 (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_OK);
    CHECK(cp_spp_dbus_deliver_rx(engine, "11:22:33:44:55:66",
                                 CP_SPP_DBUS_RX_UUID_KIND_CAFF,
                                 (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_ERR_REJECTED);
    CHECK(cp_spp_dbus_deliver_rx(engine, TEST_PEER,
                                 CP_SPP_DBUS_RX_UUID_KIND_CAFE,
                                 (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_ERR_REJECTED);
    CHECK(cp_spp_dbus_deliver_rx(engine, "not-a-mac",
                                 CP_SPP_DBUS_RX_UUID_KIND_CAFF,
                                 (const uint8_t *)"rx", 2) ==
          CP_SPP_DBUS_ERR_REJECTED);
    CHECK(cap.rx_calls == 2);

    CHECK(cp_spp_dbus_stop(engine, now) == CP_SPP_DBUS_OK);
    for (unsigned i = 0; i < 2000 &&
         cp_spp_dbus_listener_owned(engine) == 1 &&
         cap.fatal_calls == 0; i++)
        tick_once(engine, &now, 10);
    CHECK(cp_spp_dbus_listener_owned(engine) == 0);
    CHECK(cp_spp_dbus_state_get(engine) == CP_SPP_DBUS_STATE_LISTENING);
    CHECK(cap.removed >= 1);

    cp_spp_dbus_close(engine);
    close(cmd_pipe[1]);
    kill(child, SIGTERM);
    (void)waitpid(child, NULL, 0);
    puts("PASS: cp_spp_dbus private-bus lifecycle, bounded TX, RX and stop");
    return 0;
}
