/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Offline runtime broker integration: real private dbus-daemon, first-party
 * mock OEM service, private helper socket, credential-checked hook receiver
 * and synthetic phone link frames.  No host/system bus, OEM binary, chip,
 * hardware or network is used.
 */
#define _GNU_SOURCE
#include "cp_runtime_broker.h"

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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define main original_session_tests
#include "test_carplay_session.c"
#undef main

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define TEST_SERVICE "SCOURGE Runtime SPP"
#define TEST_PEER "AA:BB:CC:DD:EE:FF"
#define TEST_PEER2 "11:22:33:44:55:66"
#define SERIAL_PATH1 \
    "/dev_AA_BB_CC_DD_EE_FF/serial_00000000_DECA_FADE_DECA_DEAFDECACAFF"
#define SERIAL_PATH2 \
    "/dev_11_22_33_44_55_66/serial_00000000_DECA_FADE_DECA_DEAFDECACAFF"
_Static_assert(sizeof(SERIAL_PATH1) - 1u == 66u, "serial path length");
_Static_assert(sizeof(SERIAL_PATH2) - 1u == 66u, "serial path length");

/* ------------------------------------------------------------------ */
/* mock OEM service child                                             */
/* ------------------------------------------------------------------ */

typedef struct {
    DBusConnection *conn;
    int ready_fd;
    int data_fd;
    int cmd_fd;
    const char *serial_path;
    bool listener;
    bool emitted;
    bool no_connect;
    int send_delay;
    int pending_delay;
    DBusMessage *pending_reply;
} mock_t;

static const char *adapter_xml =
    "<?xml version=\"1.0\"?>"
    "<node><interface name=\"org.bluez.Adapter.Adapter.v2_0\">"
    "<method name=\"ListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/></method>"
    "<method name=\"StopListenSPP\">"
    "<arg type=\"s\" direction=\"in\"/><arg type=\"s\" direction=\"in\"/>"
    "<arg type=\"i\" direction=\"out\"/></method>"
    "</interface></node>";

static const char *serial_xml =
    "<?xml version=\"1.0\"?>"
    "<node><interface name=\"org.bluez.SerialPort.SerialPort.v2_0\">"
    "<method name=\"SendData\">"
    "<arg type=\"ay\" direction=\"in\"/><arg type=\"i\" direction=\"out\"/>"
    "</method>"
    "<method name=\"Disconnect\">"
    "<arg type=\"i\" direction=\"out\"/></method>"
    "<signal name=\"onStatusAttributeChanged\">"
    "<arg type=\"s\" direction=\"out\"/><arg type=\"s\" direction=\"out\"/>"
    "</signal></interface></node>";

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
        if (path != NULL && strcmp(path, CP_SPP_DBUS_ADAPTER_PATH) == 0)
            mock_reply_string(conn, msg, adapter_xml);
        else if (path != NULL && strcmp(path, mock->serial_path) == 0)
            mock_reply_string(conn, msg, serial_xml);
        else
            return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, CP_SPP_DBUS_ADAPTER_PATH) == 0 &&
        dbus_message_is_method_call(msg, CP_SPP_DBUS_ADAPTER_INTERFACE,
                                    "ListenSPP")) {
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
        }
        if (!ok) {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        mock->listener = true;
        mock->emitted = false;
        mock_reply_int(conn, msg, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, CP_SPP_DBUS_ADAPTER_PATH) == 0 &&
        dbus_message_is_method_call(msg, CP_SPP_DBUS_ADAPTER_INTERFACE,
                                    "StopListenSPP")) {
        const char *signature = dbus_message_get_signature(msg);
        if (signature != NULL &&
            (strcmp(signature, "") == 0 || strcmp(signature, "ss") == 0)) {
            mock->listener = false;
            mock->emitted = false;
            mock_reply_int(conn, msg, 0);
        } else {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, mock->serial_path) == 0 &&
        dbus_message_is_method_call(msg, CP_SPP_DBUS_SERIAL_INTERFACE,
                                    "SendData")) {
        DBusMessageIter iter, array;
        if (!dbus_message_iter_init(msg, &iter) ||
            dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY ||
            dbus_message_iter_get_element_type(&iter) != DBUS_TYPE_BYTE) {
            mock_reply_error(conn, msg, DBUS_ERROR_INVALID_ARGS);
            return DBUS_HANDLER_RESULT_HANDLED;
        }
        dbus_message_iter_recurse(&iter, &array);
        uint8_t bytes[CP_SPP_DBUS_MAX_TX_BYTES];
        size_t used = 0;
        while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_BYTE &&
               used < sizeof(bytes)) {
            unsigned char byte = 0;
            dbus_message_iter_get_basic(&array, &byte);
            bytes[used++] = byte;
            dbus_message_iter_next(&array);
        }
        if (used > 0) (void)write(mock->data_fd, bytes, used);
        if (mock->send_delay > 0) {
            if (mock->pending_reply != NULL)
                dbus_message_unref(mock->pending_reply);
            mock->pending_reply = dbus_message_ref(msg);
            mock->pending_delay = mock->send_delay;
        } else {
            mock_reply_int(conn, msg, 0);
        }
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    if (path != NULL && strcmp(path, mock->serial_path) == 0 &&
        dbus_message_is_method_call(msg, CP_SPP_DBUS_SERIAL_INTERFACE,
                                    "Disconnect")) {
        mock_reply_int(conn, msg, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void mock_emit_status(DBusConnection *conn, const char *path,
                             const char *status)
{
    DBusMessage *message = dbus_message_new_signal(
        path, CP_SPP_DBUS_SERIAL_INTERFACE,
        CP_SPP_DBUS_SERIAL_SIGNAL_STATUS);
    DBusMessageIter iter;
    if (message == NULL) return;
    dbus_message_iter_init_append(message, &iter);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &path);
    (void)dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &status);
    (void)dbus_connection_send(conn, message, NULL);
    dbus_connection_flush(conn);
    dbus_message_unref(message);
}

static void mock_serial_service(const char *path, char *out, size_t cap)
{
    snprintf(out, cap, "%s%s", CP_SPP_DBUS_SERIAL_SERVICE_PREFIX, path + 1);
    for (char *p = out; *p; p++)
        if (*p == '/') *p = '.';
}

static void mock_child(const char *address, int ready_fd, int data_fd,
                       int cmd_fd, bool no_connect, bool attach_only)
{
    DBusError error;
    dbus_error_init(&error);
    DBusConnection *conn = dbus_connection_open_private(address, &error);
    if (conn == NULL) _exit(2);
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    if (!dbus_bus_register(conn, &error)) _exit(3);
    mock_t mock = {0};
    mock.conn = conn;
    mock.ready_fd = ready_fd;
    mock.data_fd = data_fd;
    mock.cmd_fd = cmd_fd;
    mock.serial_path = SERIAL_PATH1;
    mock.no_connect = no_connect;
    (void)fcntl(cmd_fd, F_SETFL, O_NONBLOCK);
    if (!dbus_connection_add_filter(conn, mock_filter, &mock, NULL)) _exit(4);
    int request = dbus_bus_request_name(conn, CP_SPP_DBUS_ADAPTER_SERVICE,
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(5);
    char service1[CP_SPP_DBUS_MAX_NAME], service2[CP_SPP_DBUS_MAX_NAME];
    mock_serial_service(SERIAL_PATH1, service1, sizeof(service1));
    mock_serial_service(SERIAL_PATH2, service2, sizeof(service2));
    dbus_error_free(&error);
    dbus_error_init(&error);
    request = dbus_bus_request_name(conn, service1,
        DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(7);
    dbus_error_free(&error);
    dbus_error_init(&error);
    if (!attach_only) {
        request = dbus_bus_request_name(conn, service2,
            DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
        if (request != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) _exit(8);
        dbus_error_free(&error);
    }
    char ready = 'R';
    (void)write(ready_fd, &ready, 1);
    close(ready_fd);
    if (getenv("CP_RUNTIME_DEBUG") == NULL) {
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0) {
            (void)dup2(nullfd, STDOUT_FILENO);
            (void)dup2(nullfd, STDERR_FILENO);
            if (nullfd > STDERR_FILENO) close(nullfd);
        }
    }
    for (;;) {
        char command = 0;
        if (read(cmd_fd, &command, 1) == 1) {
            if (command == 'P') mock.serial_path = SERIAL_PATH2;
            if (command == 'O') {
                DBusError e;
                dbus_error_init(&e);
                (void)dbus_bus_release_name(conn,
                    CP_SPP_DBUS_ADAPTER_SERVICE, &e);
                dbus_error_free(&e);
            }
            if (command == 'D') mock.send_delay = 200;
            if (command == 'C') mock.no_connect = false;
            if (command == 'R') {
                mock_emit_status(conn, mock.serial_path, "disconnected");
                usleep(50000);
                mock_emit_status(conn, mock.serial_path, "connected");
            }
        }
        if (mock.pending_reply != NULL) {
            if (--mock.pending_delay <= 0) {
                mock_reply_int(conn, mock.pending_reply, 0);
                dbus_message_unref(mock.pending_reply);
                mock.pending_reply = NULL;
            }
        }
        (void)dbus_connection_read_write_dispatch(conn, 20);
        if (mock.listener && !mock.emitted && !mock.no_connect &&
            !attach_only) {
            mock_emit_status(conn, mock.serial_path, "connected");
            mock.emitted = true;
        }
    }
}

/* ------------------------------------------------------------------ */
/* private helper server                                              */
/* ------------------------------------------------------------------ */

static bool g_helper_disabled;
static bool g_helper_paused;

typedef struct {
    int listener;
    int client;
    uint8_t incoming[CP_OEM_AUTH_RPC_REQUEST_LEN +
                     CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD];
    uint8_t outgoing[CP_OEM_AUTH_RPC_RESPONSE_LEN +
                     CP_OEM_AUTH_ASYNC_MAX_CERTIFICATE];
    cp_oem_auth_rpc_request request;
    bool have_request;
    int delay;
    unsigned operations;
} helper_t;

static void helper_service(helper_t *helper)
{
    if (g_helper_disabled) return;
    if (helper->client < 0) {
        int fd = accept4(helper->listener, NULL, NULL,
                         SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd >= 0) helper->client = fd;
    }
    if (helper->client >= 0 && !helper->have_request) {
        ssize_t n = recv(helper->client, helper->incoming,
                         sizeof(helper->incoming), MSG_DONTWAIT);
        if (n > 0 && (size_t)n >= CP_OEM_AUTH_RPC_REQUEST_LEN &&
            cp_oem_auth_rpc_request_decode(helper->incoming,
                CP_OEM_AUTH_RPC_REQUEST_LEN, &helper->request)) {
            helper->have_request = true;
            helper->delay = 5;
        }
    }
    if (g_helper_paused) return;
    if (helper->have_request && --helper->delay <= 0) {
        cp_oem_auth_rpc_response response;
        memset(&response, 0, sizeof(response));
        response.magic = CP_OEM_AUTH_RPC_MAGIC;
        response.version = CP_OEM_AUTH_RPC_VERSION;
        response.op = helper->request.op;
        response.request_id = helper->request.request_id;
        response.payload_len =
            helper->request.op == CP_OEM_AUTH_RPC_OP_CERTIFICATE ? 300u : 128u;
        cp_oem_auth_rpc_response_encode(helper->outgoing, &response);
        memset(helper->outgoing + CP_OEM_AUTH_RPC_RESPONSE_LEN, 0x55,
               response.payload_len);
        ssize_t n = send(helper->client, helper->outgoing,
            CP_OEM_AUTH_RPC_RESPONSE_LEN + response.payload_len,
            MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n == (ssize_t)(CP_OEM_AUTH_RPC_RESPONSE_LEN +
                           response.payload_len)) {
            helper->operations++;
            close(helper->client);
            helper->client = -1;
            helper->have_request = false;
        }
    }
}

/* ------------------------------------------------------------------ */
/* runtime fixture                                                    */
/* ------------------------------------------------------------------ */

static cp_runtime_broker g_broker;
static cp_spp_socket g_rx;
static int g_sender = -1;
static int g_data[2] = {-1, -1};
static int g_cmd[2] = {-1, -1};
static int g_ready[2] = {-1, -1};
static pid_t g_child = -1;
static char g_rx_path[120];
static char g_helper_path[120];
static helper_t g_helper;
static cp_boot_consent g_consent = CP_BOOT_CONSENT_GRANTED;
static bool g_usb_active;
static uint64_t g_now;
static phone_t g_phone;
static cp_session_config_t g_session_config;
static bool g_challenge_sent;
static bool g_ignore_phone_data;
static bool g_tolerate_link_errors;
static bool g_phone_auth_fail;
static bool g_mock_no_connect;
static bool g_mock_attach_only;
static uint32_t g_restart_epoch_target;

static cp_boot_consent runtime_consent(void *context, uint64_t now_ms)
{
    (void)context;
    (void)now_ms;
    return g_consent;
}

static bool runtime_usb(void *context, const char *bluetooth_id,
                        const char *usb_id)
{
    (void)context;
    (void)bluetooth_id;
    (void)usb_id;
    return g_usb_active;
}

static void start_helper(void)
{
    char directory[] = "/tmp/opencode/cp-runtime-helper-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    snprintf(g_helper_path, sizeof(g_helper_path), "%s/helper.sock", directory);
    g_helper.listener = socket(AF_UNIX,
        SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(g_helper.listener >= 0);
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, g_helper_path, strlen(g_helper_path) + 1);
    CHECK(bind(g_helper.listener, (struct sockaddr *)&address,
               sizeof(address)) == 0);
    CHECK(chmod(g_helper_path, 0600) == 0);
    CHECK(listen(g_helper.listener, 4) == 0);
    g_helper.client = -1;
}

static void start_mock(const char *address)
{
    CHECK(pipe2(g_ready, O_CLOEXEC) == 0);
    CHECK(pipe2(g_data, O_CLOEXEC) == 0);
    CHECK(pipe2(g_cmd, O_CLOEXEC) == 0);
    g_child = fork();
    CHECK(g_child >= 0);
    if (g_child == 0) {
        close(g_ready[0]); close(g_data[0]); close(g_cmd[1]);
        mock_child(address, g_ready[1], g_data[1], g_cmd[0],
                   g_mock_no_connect, g_mock_attach_only);
        _exit(6);
    }
    close(g_ready[1]); close(g_data[1]); close(g_cmd[0]);
    char ready = 0;
    CHECK(read(g_ready[0], &ready, 1) == 1 && ready == 'R');
    close(g_ready[0]);
    (void)fcntl(g_data[0], F_SETFL, O_NONBLOCK);
}

static void start_rx(void)
{
    char directory[] = "/tmp/opencode/cp-runtime-rx-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    CHECK(chmod(directory, 0700) == 0);
    snprintf(g_rx_path, sizeof(g_rx_path), "%s/rx.sock", directory);
    cp_spp_socket_init(&g_rx);
    CHECK(cp_spp_socket_open_fixture(&g_rx, g_rx_path) == 0);
    g_sender = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(g_sender >= 0);
}

static void send_frame(const char *peer, const uint8_t *data, size_t length)
{
    uint8_t frame[CP_SPP_SOCKET_MAX_FRAME];
    struct sockaddr_un address = {0};
    CHECK(length > 0 && length <= CP_SPP_RX_MAX_PAYLOAD);
    memcpy(frame, CP_SPP_RX_MAGIC, CP_SPP_RX_MAGIC_LEN);
    uint32_t len = (uint32_t)length;
    frame[8] = (uint8_t)(len & 0xffu);
    frame[9] = (uint8_t)((len >> 8) & 0xffu);
    frame[10] = (uint8_t)((len >> 16) & 0xffu);
    frame[11] = (uint8_t)((len >> 24) & 0xffu);
    memset(frame + CP_SPP_RX_OFF_PEER, 0, CP_SPP_RX_PEER_LEN);
    memcpy(frame + CP_SPP_RX_OFF_PEER, peer, CP_SPP_RX_PEER_LEN);
    frame[CP_SPP_RX_OFF_UUID_KIND] = CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF;
    frame[CP_SPP_RX_OFF_RESERVED] = 0;
    frame[CP_SPP_RX_OFF_RESERVED + 1] = 0;
    memcpy(frame + CP_SPP_RX_HEADER_SIZE, data, length);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, g_rx_path, strlen(g_rx_path) + 1);
    CHECK(sendto(g_sender, frame, CP_SPP_RX_HEADER_SIZE + length, 0,
                 (struct sockaddr *)&address, sizeof(address)) ==
          (ssize_t)(CP_SPP_RX_HEADER_SIZE + length));
}

static void send_phone_output(void)
{
    for (;;) {
        const uint8_t *data = NULL;
        size_t len = cp_link_output_peek(&g_phone.link, &data);
        if (len == 0 || data == NULL) break;
        size_t chunk = len > 1000u ? 1000u : len;
        send_frame(TEST_PEER, data, chunk);
        CHECK(cp_link_output_consume(&g_phone.link, chunk) == CP_LINK_OK);
    }
}

static void drain_mock_data(void)
{
    uint8_t buffer[4096];
    while (read(g_data[0], buffer, sizeof(buffer)) > 0) {}
}

static void feed_phone_from_mock(void)
{
    uint8_t buffer[4096];
    if (g_ignore_phone_data) {
        if (g_broker.epoch <= g_restart_epoch_target) {
            drain_mock_data();
            return;
        }
        drain_mock_data();
        g_ignore_phone_data = false;
    }
    for (;;) {
        ssize_t n = read(g_data[0], buffer, sizeof(buffer));
        if (n <= 0) break;
        int feed = cp_link_feed(&g_phone.link, buffer, (size_t)n, g_now);
        if (feed != CP_LINK_OK && !g_tolerate_link_errors)
            CHECK(feed == CP_LINK_OK);
    }
}

static void reset_phone(void)
{
    g_ignore_phone_data = true;
    g_restart_epoch_target = g_broker.epoch;
    phone_init(&g_phone, 32u, 8u, true, true, g_phone_auth_fail);
    g_phone.sent_challenge = true;
    g_challenge_sent = false;
}

static void runtime_pump(unsigned steps)
{
    for (unsigned i = 0; i < steps; i++) {
        g_now += 5u;
        helper_service(&g_helper);
        send_phone_output();
        struct pollfd pfds[3];
        size_t n = cp_runtime_broker_pollfds(&g_broker, pfds, 3);
        (void)poll(pfds, n, 0);
        (void)cp_runtime_broker_tick(&g_broker, pfds, n, g_now);
        feed_phone_from_mock();
        phone_service(&g_phone, g_now);
        if (g_phone.got_cert && !g_challenge_sent) {
            CHECK(phone_queue_hex(&g_phone,
                "4040002aaa0200240000"
                "01010101010101010101010101010101"
                "01010101010101010101010101010101", 7u));
            g_challenge_sent = true;
        }
        CHECK(cp_link_tick(&g_phone.link, g_now) == CP_LINK_OK);
        usleep(1000);
    }
}

static void broker_config(cp_runtime_broker_config_t *config,
                          const char *address)
{
    memset(config, 0, sizeof(*config));
    base_session_config(&g_session_config);
    g_session_config.link.max_payload = 64u;
    g_session_config.link.max_outgoing = 8u;
    config->experimental = 1;
    config->enabled = 1;
    config->spp.bus_address = address;
    config->spp.service_name = TEST_SERVICE;
    config->spp.expected_owner_uid_set = 1;
    config->spp.expected_owner_uid = (uint32_t)geteuid();
    config->spp.control_timeout_ms = 3000u;
    config->spp.send_timeout_ms = 1000u;
    config->spp.max_tx_bytes = CP_SPP_DBUS_MAX_TX_BYTES;
    config->spp.experimental = 1;
    config->auth.experimental = true;
#ifdef CP_OEM_AUTH_ASYNC_TEST
    config->auth.socket_path = g_helper_path;
    config->auth.expected_peer_uid = (uint32_t)geteuid();
#endif
    config->session = g_session_config;
    config->role = CP_LINK_ROLE_INITIATOR;
    config->consent = runtime_consent;
    config->usb_active = runtime_usb;
    config->ap_readback.on_air = true;
    config->ap_readback.ssid = g_session_config.boot.ap_ssid;
    config->ap_readback.channel = g_session_config.boot.ap_channel;
    config->ap_readback.security = g_session_config.boot.ap_security;
    config->ap_readback.link_local = g_session_config.boot.link_local;
    config->have_ap_readback = 1;
    config->init_timeout_ms = 30000u;
    config->drain_timeout_ms = 2000u;
    config->max_tx_chunks_per_tick = 2u;
    config->max_rx_per_tick = 8u;
    config->rx = &g_rx;
}

static bool wait_for_broker(const char *scenario, unsigned limit)
{
    for (unsigned i = 0; i < limit; i++) {
        runtime_pump(1);
        cp_session_state_t state = cp_session_state(&g_broker.session);
        if (g_broker.state == CP_RUNTIME_BROKER_FATAL ||
            g_broker.state == CP_RUNTIME_BROKER_BLOCKED)
            return false;
        if (state == CP_SESSION_STATE_AUTHENTICATED ||
            state == CP_SESSION_STATE_AWAITING_RECEIVER ||
            cp_session_auth_state(&g_broker.session) ==
                CP_BOOT_AUTH_SUCCEEDED)
            return true;
        (void)scenario;
    }
    return false;
}

static bool wait_for_pending_helper(unsigned limit)
{
    for (unsigned i = 0; i < limit; i++) {
        runtime_pump(1);
        if (g_broker.state == CP_RUNTIME_BROKER_FATAL ||
            g_broker.state == CP_RUNTIME_BROKER_BLOCKED)
            return false;
        if (g_broker.session_inited && g_helper.have_request &&
            cp_session_auth_state(&g_broker.session) !=
                CP_BOOT_AUTH_SUCCEEDED)
            return true;
    }
    return false;
}

static void cleanup(void)
{
    cp_runtime_broker_destroy(&g_broker, g_now);
    if (g_sender >= 0) close(g_sender);
    cp_spp_socket_close(&g_rx);
    if (g_child > 0) {
        kill(g_child, SIGTERM);
        (void)waitpid(g_child, NULL, 0);
    }
    if (g_data[0] >= 0) close(g_data[0]);
    if (g_cmd[1] >= 0) close(g_cmd[1]);
    if (g_helper.client >= 0) close(g_helper.client);
    if (g_helper.listener >= 0) close(g_helper.listener);
    unlink(g_helper_path);
    unlink(g_rx_path);
}

int main(int argc, char **argv)
{
    CHECK(argc >= 3);
    const char *address = argv[1];
    const char *scenario = argv[2];

    /* The leak scenario intentionally never opens libdbus: it exercises only
     * init/destroy so the Python runner can enable ASan leak detection for the
     * runtime's own storage path while normal libdbus scenarios use
     * detect_leaks=0 (libdbus and the mock service keep process-global caches). */
    if (strcmp(scenario, "leakinit") == 0) {
        cp_runtime_broker_config_t config;
        snprintf(g_helper_path, sizeof(g_helper_path), "/nonexistent-helper");
        broker_config(&config, "unix:path=/nonexistent-bus");
        config.rx = NULL;
        CHECK(cp_runtime_broker_init(&g_broker, &config) == CP_SESSION_OK);
        cp_runtime_broker_destroy(&g_broker, 0);
        puts("PASS: runtime broker init/destroy ASan leak check");
        return 0;
    }

    if (strcmp(scenario, "no_connect") == 0 ||
        strcmp(scenario, "delayed") == 0 ||
        strcmp(scenario, "drain_boundary") == 0)
        g_mock_no_connect = true;
    if (strcmp(scenario, "attach_early") == 0)
        g_mock_attach_only = true;
    if (strcmp(scenario, "epoch_pending_feed") == 0 ||
        strcmp(scenario, "epoch_pending_rx") == 0)
        g_helper_paused = true;

    start_helper();
    start_rx();
    start_mock(address);

    /* Deliberate fixture failure: the parent exits without cleanup so the
     * Python runner must kill the whole fixture process group, including the
     * mock child that keeps the captured pipes open under CP_RUNTIME_DEBUG. */
    if (strcmp(scenario, "failchild") == 0) {
        const char *pidfile = getenv("CP_RUNTIME_CHILD_PID_FILE");
        if (pidfile != NULL) {
            FILE *file = fopen(pidfile, "w");
            if (file != NULL) {
                fprintf(file, "%ld\n", (long)g_child);
                fclose(file);
            }
        }
        _exit(7);
    }

    cp_runtime_broker_config_t config;
    broker_config(&config, address);
    if (strcmp(scenario, "timeout") == 0) {
        config.init_timeout_ms = 150u;
        g_helper_disabled = true;
    }
    if (strcmp(scenario, "no_connect") == 0)
        config.init_timeout_ms = 200u;
    if (strcmp(scenario, "delayed") == 0) {
        config.init_timeout_ms = 5000u;
        config.drain_timeout_ms = 10000u;
    }
    if (strcmp(scenario, "drain_boundary") == 0) {
        config.init_timeout_ms = 5000u;
        config.drain_timeout_ms = 1u;
    }
    if (strcmp(scenario, "epoch_pending_feed") == 0 ||
        strcmp(scenario, "epoch_pending_rx") == 0)
        config.init_timeout_ms = 5000u;
    if (strcmp(scenario, "ap") == 0 ||
        strcmp(scenario, "ap_pre") == 0 ||
        strcmp(scenario, "epoch") == 0 ||
        strcmp(scenario, "overflow") == 0)
        config.init_timeout_ms = 10000u;
    if (strcmp(scenario, "backpressure") == 0) {
        char command = 'D';
        CHECK(write(g_cmd[1], &command, 1) == 1);
        config.spp.send_timeout_ms = 6000u;
        g_tolerate_link_errors = true;
    }
    CHECK(cp_runtime_broker_init(&g_broker, &config) == CP_SESSION_OK);
    g_consent = CP_BOOT_CONSENT_GRANTED;
    g_usb_active = false;
    g_now = 1000u;
    if (strcmp(scenario, "ap_pre") == 0) {
        cp_boot_ap_readback off = {0};
        cp_boot_ap_readback on = {
            .on_air = true,
            .ssid = "SCOURGE-AP",
            .channel = 36u,
            .security = CP_WIRE_WIFI_WPA2,
            .link_local = "fe80::1"
        };
        cp_boot_ap_readback bad = {
            .on_air = true,
            .ssid = "",
            .channel = 0u,
            .security = CP_WIRE_WIFI_WPA2
        };
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &off,
              g_now + 10u) == CP_SESSION_OK);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &bad,
              g_now + 20u) == CP_SESSION_ERR_CONFIG);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on,
              g_now - 1u) == CP_SESSION_ERR_CLOCK_ROLLBACK);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on,
              g_now + 30u) == CP_SESSION_OK);
        CHECK(g_broker.cfg.ap_readback.on_air);
        g_now += 40u;
    }
    g_auth.cert_len = 300u;
    g_auth.signature_len = 128u;
    g_phone_auth_fail = strcmp(scenario, "authfail") == 0;
    phone_init(&g_phone, 32u, 8u, true, true, g_phone_auth_fail);
    g_phone.sent_challenge = true;
    g_challenge_sent = false;
    if (strcmp(scenario, "attach_early") == 0) {
        /* Queue the phone's first link frame before the broker starts reading.
         * Attach must preserve it rather than discarding it as stale. */
        send_phone_output();
    }
    CHECK(cp_runtime_broker_start(&g_broker, g_now) == CP_SESSION_OK);

    if (strcmp(scenario, "no_connect") == 0) {
        CHECK(g_broker.init_deadline_ms != 0u);
        CHECK(cp_runtime_broker_next_deadline(&g_broker) ==
              g_broker.init_deadline_ms);
        for (unsigned i = 0; i < 200 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.init_timed_out);
        CHECK(g_broker.fatal);
        CHECK(g_broker.restart_required);
        CHECK(g_broker.spp == NULL);
        CHECK(g_broker.epoch == 0u);
        CHECK(!g_broker.session_inited);
        CHECK(g_broker.init_deadline_ms == 0u);
        {
            uint8_t aa05[CP_WIRE_MAX_FRAME];
            size_t len = hex2bin(k_aa05, aa05, sizeof(aa05));
            send_frame(TEST_PEER, aa05, len);
        }
        runtime_pump(10);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.epoch == 0u);
        CHECK(cp_session_auth_state(&g_broker.session) !=
              CP_BOOT_AUTH_SUCCEEDED);
        cleanup();
        puts("PASS: runtime broker WAIT_SPP total deadline and late AA05 fail closed");
        return 0;
    }

    if (strcmp(scenario, "timeout") == 0) {
        for (unsigned i = 0; i < 200 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        if (!g_broker.init_timed_out)
            fprintf(stderr, "DEBUG timeout state=%d err=%s epoch=%u auth=%d sess=%d init=%llu now=%llu\n",
                    (int)g_broker.state, cp_runtime_broker_error(&g_broker),
                    g_broker.epoch, (int)cp_session_auth_state(&g_broker.session),
                    (int)cp_session_state(&g_broker.session),
                    (unsigned long long)g_broker.init_deadline_ms,
                    (unsigned long long)g_now);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.init_timed_out);
        CHECK(g_broker.fatal);
        CHECK(g_broker.restart_required);
        CHECK(g_broker.spp == NULL);
        CHECK(cp_runtime_broker_epoch(&g_broker) == 1u);
        CHECK(cp_runtime_broker_state(&g_broker) ==
              CP_RUNTIME_BROKER_FATAL);
        /* A late AA05 after the total deadline must not revive the session. */
        {
            uint8_t aa05[CP_WIRE_MAX_FRAME];
            size_t len = hex2bin(k_aa05, aa05, sizeof(aa05));
            send_frame(TEST_PEER, aa05, len);
        }
        runtime_pump(10);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.init_timed_out);
        CHECK(g_broker.init_deadline_ms == 0u);
        CHECK(cp_runtime_broker_epoch(&g_broker) == 1u);
        CHECK(cp_session_auth_state(&g_broker.session) !=
              CP_BOOT_AUTH_SUCCEEDED);
        cleanup();
        puts("PASS: runtime broker bounded init timeout stays fatal");
        return 0;
    }

    if (strcmp(scenario, "authfail") == 0) {
        for (unsigned i = 0; i < 3000 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.auth_failed);
        CHECK(g_broker.fatal);
        CHECK(g_broker.restart_required);
        CHECK(g_broker.spp == NULL);
        uint32_t epoch = g_broker.epoch;
        runtime_pump(10);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.epoch == epoch);
        cleanup();
        puts("PASS: runtime broker auth failure is terminal until a new SPP epoch");
        return 0;
    }
    if (strcmp(scenario, "backpressure") == 0) {
        for (unsigned i = 0; i < 400; i++) runtime_pump(1);
        CHECK(g_broker.stats.tx_backpressure > 0u);
        CHECK(g_broker.state != CP_RUNTIME_BROKER_FATAL);
        cleanup();
        puts("PASS: runtime broker bounds SPP TX while servicing its loop");
        return 0;
    }
    if (strcmp(scenario, "drain_boundary") == 0) {
        for (unsigned i = 0; i < 8; i++) {
            uint8_t stale[] = "STALE-BOUNDARY";
            send_frame(TEST_PEER, stale, sizeof(stale) - 1u);
        }
        for (unsigned i = 0; i < 10; i++) runtime_pump(1);
        {
            char command = 'C';
            CHECK(write(g_cmd[1], &command, 1) == 1);
        }
        for (unsigned i = 0; i < 200 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.fatal);
        CHECK(strstr(cp_runtime_broker_error(&g_broker),
                     "stale_rx_drain_timeout") != NULL);
        CHECK(!g_broker.session_inited);
        CHECK(g_broker.spp == NULL);
        cleanup();
        puts("PASS: expired drain deadline fails closed before EAGAIN");
        return 0;
    }

    if (strcmp(scenario, "epoch_pending_feed") == 0 ||
        strcmp(scenario, "epoch_pending_rx") == 0) {
        CHECK(wait_for_pending_helper(4000));
        uint32_t epoch = cp_runtime_broker_epoch(&g_broker);
        uint32_t generation = cp_session_generation(&g_broker.session);
        const char *peer = cp_runtime_broker_peer(&g_broker);
        uint64_t last = g_broker.last_now_ms;
        uint64_t deadline = g_broker.init_deadline_ms;
        uint8_t byte = 0;
        CHECK(epoch >= 1u);
        CHECK(deadline > last + 100u);
        CHECK(g_helper.have_request);
        CHECK(cp_session_auth_state(&g_broker.session) !=
              CP_BOOT_AUTH_SUCCEEDED);

        /* Stale epoch before and past the deadline must be side-effect free:
         * no clock, peer, epoch, state or init-deadline change. */
        uint64_t before = deadline - 50u;
        uint64_t past = deadline + 50u;
        CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch - 1u, peer,
              CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, &byte,
              sizeof(byte), before) == 0);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, before,
              epoch - 1u, generation, peer, false) ==
              CP_SESSION_ERR_GENERATION_MISMATCH);
        CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch - 1u, peer,
              CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, &byte,
              sizeof(byte), past) == 0);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, past,
              epoch - 1u, generation, peer, false) ==
              CP_SESSION_ERR_GENERATION_MISMATCH);
        CHECK(g_broker.state != CP_RUNTIME_BROKER_FATAL);
        CHECK(!g_broker.fatal);
        CHECK(!g_broker.session_failed);
        CHECK(g_broker.session_inited);
        CHECK(g_broker.spp != NULL);
        CHECK(cp_runtime_broker_epoch(&g_broker) == epoch);
        CHECK(strcmp(cp_runtime_broker_peer(&g_broker), peer) == 0);
        CHECK(g_broker.last_now_ms == last);
        CHECK(g_broker.init_deadline_ms == deadline);
        CHECK(cp_session_generation(&g_broker.session) == generation);

        if (strcmp(scenario, "epoch_pending_feed") == 0) {
            CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch, peer,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, &byte,
                  sizeof(byte), past) == -1);
        } else {
            CHECK(cp_runtime_broker_set_receiver_active(&g_broker, past,
                  epoch, generation, peer, false) ==
                  CP_SESSION_ERR_TIMEOUT);
        }
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.fatal);
        CHECK(g_broker.init_timed_out);
        CHECK(g_broker.init_deadline_ms == 0u);
        CHECK(!g_broker.session_inited);
        CHECK(g_broker.spp == NULL);
        cleanup();
        puts("PASS: stale epoch is inert before/after the new-session deadline");
        return 0;
    }

    if (strcmp(scenario, "delayed") == 0) {
        uint64_t deadline0 = g_broker.init_deadline_ms;
        CHECK(deadline0 != 0u);
        for (unsigned i = 0; i < 20; i++) {
            uint8_t stale[] = "STALE-DRAIN";
            send_frame(TEST_PEER, stale, sizeof(stale) - 1u);
            runtime_pump(1);
        }
        CHECK(g_broker.state == CP_RUNTIME_BROKER_WAIT_SPP);
        CHECK(g_broker.init_deadline_ms == deadline0);
        {
            char command = 'C';
            CHECK(write(g_cmd[1], &command, 1) == 1);
        }
        for (unsigned i = 0; i < 200 && g_broker.epoch == 0u &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state != CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.epoch == 1u);
        CHECK(g_broker.init_deadline_ms == deadline0);
        CHECK(g_broker.drain_deadline_ms == deadline0);
        CHECK(wait_for_broker(scenario, 6000));
        runtime_pump(1);
        CHECK(g_broker.init_deadline_ms == 0u);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_ACTIVE);
        CHECK(!g_broker.init_timed_out);
        CHECK(g_broker.stats.rx_dropped >= 20u);
        cleanup();
        puts("PASS: runtime broker keeps one total init budget across delayed connect and drain");
        return 0;
    }

    if (!wait_for_broker(scenario, 4000)) {
        fprintf(stderr, "DEBUG state=%d err=%s epoch=%u auth=%d sess=%d spp=%d txc=%u rxp=%u drop=%u stub=%u\n",
                (int)g_broker.state, cp_runtime_broker_error(&g_broker),
                g_broker.epoch, (int)cp_session_auth_state(&g_broker.session),
                (int)cp_session_state(&g_broker.session),
                (int)cp_spp_dbus_state_get(g_broker.spp),
                g_broker.stats.tx_chunks, g_broker.stats.rx_packets,
                g_broker.stats.rx_dropped, g_broker.stats.stub_acks);
        fprintf(stderr, "DEBUG last_session_err=%d helper_ops=%u have_req=%d helper_client=%d\n",
                (int)cp_session_last_error(&g_broker.session),
                g_helper.operations, g_helper.have_request ? 1 : 0,
                g_helper.client);
        fprintf(stderr, "DEBUG helper_state=%d op=%d comp_err=%d comp_op=%d comp_req=%u broker_req=%u\n",
                (int)g_broker.auth.helper.state, (int)g_broker.auth.helper.op,
                (int)g_broker.auth.helper.completion.error,
                (int)g_broker.auth.helper.completion.op,
                g_broker.auth.helper.completion.request_id,
                g_broker.auth.request_id);
        fprintf(stderr, "DEBUG spp_send_timeout=%u spp_control=%u\n",
                g_broker.cfg.spp.send_timeout_ms,
                g_broker.cfg.spp.control_timeout_ms);
        fprintf(stderr, "DEBUG phone linkup=%d ident=%d cert=%d resp=%d sent_chal=%d sent_auth=%d tx=%u rxlen=%zu now=%llu sess_auth=%d\n",
                g_phone.link_up, g_phone.got_ident, g_phone.got_cert,
                g_phone.got_resp, g_phone.sent_challenge, g_phone.sent_auth,
                g_phone.tx_count, g_phone.rx_len,
                (unsigned long long)g_now,
                (int)cp_session_auth_state(&g_broker.session));
        CHECK(0);
    }
    CHECK(cp_runtime_broker_state(&g_broker) ==
          CP_RUNTIME_BROKER_ACTIVE ||
          g_broker.state == CP_RUNTIME_BROKER_STARTING);
    CHECK(strcmp(cp_runtime_broker_peer(&g_broker), TEST_PEER) == 0);
    CHECK(cp_runtime_broker_epoch(&g_broker) >= 1u);
    CHECK(cp_session_auth_state(&g_broker.session) ==
          CP_BOOT_AUTH_SUCCEEDED);
    if (strcmp(scenario, "attach_early") == 0) {
        CHECK(g_broker.stats.early_rx >= 1u);
        CHECK(g_broker.stats.rx_packets > 0u);
        CHECK(cp_spp_dbus_attached(g_broker.spp) == 1);
        /* The replayed buffer is gone, and a restart for a new epoch must
         * clear the old connection's bytes but keep preserving new ones. */
        CHECK(g_broker.early_count == 0u);
        CHECK(cp_runtime_broker_restart(&g_broker, g_now + 100u) ==
              CP_SESSION_OK);
        CHECK(g_broker.preserve_early_rx);
        CHECK(g_broker.early_count == 0u);
        cleanup();
        puts("PASS: runtime broker preserves early RX and resets on restart");
        return 0;
    }
    CHECK(cp_session_receiver_active(&g_broker.session) == false);
    CHECK(cp_session_projection_active(&g_broker.session) == false);
    CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
          cp_runtime_broker_epoch(&g_broker),
          cp_session_generation(&g_broker.session), TEST_PEER, false) ==
          CP_SESSION_OK);
    CHECK(cp_session_receiver_active(&g_broker.session) == false);
    CHECK(g_broker.stats.tx_chunks > 0u);
    CHECK(g_broker.stats.rx_packets > 0u);
    CHECK(!g_broker.fatal);
    if (strcmp(scenario, "reconnect_same") == 0) {
        uint32_t old_epoch = g_broker.epoch;
        uint32_t old_sessions = g_broker.stats.sessions;
        char command = 'R';
        reset_phone();
        CHECK(write(g_cmd[1], &command, 1) == 1);
        for (unsigned i = 0; i < 4000; i++) {
            runtime_pump(1);
            if (g_broker.epoch == old_epoch + 1u &&
                g_broker.state != CP_RUNTIME_BROKER_FATAL)
                break;
            CHECK(g_broker.state != CP_RUNTIME_BROKER_FATAL);
        }
        CHECK(cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u);
        CHECK(!strcmp(cp_runtime_broker_peer(&g_broker), TEST_PEER));
        CHECK(g_broker.stats.sessions > old_sessions);
        CHECK(g_broker.stats.spp_disconnects > 0u);
        cleanup();
        puts("PASS: same-peer reconnect resets per-connection state");
        return 0;
    }
    {
        struct pollfd too_many[CP_RUNTIME_BROKER_MAX_POLLFDS + 1u];
        memset(too_many, 0, sizeof(too_many));
        CHECK(cp_runtime_broker_tick(&g_broker, too_many,
              CP_RUNTIME_BROKER_MAX_POLLFDS + 1u, g_now) ==
              CP_SESSION_ERR_ARGUMENT);
        CHECK(!g_broker.fatal);
    }

    if (strcmp(scenario, "feedfail_hook") == 0 ||
        strcmp(scenario, "feedfail_socket") == 0) {
        static const uint8_t malformed[6] = {0x40u, 0x40u, 0xFFu, 0xFFu,
                                             0x43u, 0x00u};
        uint32_t epoch = cp_runtime_broker_epoch(&g_broker);
        uint32_t generation = cp_session_generation(&g_broker.session);
        CHECK(phone_queue(&g_phone, malformed, sizeof(malformed), 0u, 0u,
                          false));
        CHECK(phone_flush(&g_phone, g_now));
        const uint8_t *data = NULL;
        size_t length = cp_link_output_peek(&g_phone.link, &data);
        CHECK(length > 0u && data != NULL);
        uint32_t tx_before = g_broker.stats.tx_chunks;
        uint32_t rx_before = g_broker.stats.rx_packets;
        uint32_t drop_before = g_broker.stats.rx_dropped;
        if (strcmp(scenario, "feedfail_hook") == 0) {
            CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch, TEST_PEER,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, data, length,
                  g_now) <= 0);
            CHECK(cp_link_output_consume(&g_phone.link, length) ==
                  CP_LINK_OK);
        } else {
            send_phone_output();
            runtime_pump(1);
        }
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.fatal);
        CHECK(g_broker.session_failed);
        CHECK(g_broker.terminal_gate);
        CHECK(g_broker.spp == NULL);
        CHECK(!g_broker.session_inited);
        CHECK(g_broker.stats.tx_chunks == tx_before);
        CHECK(g_broker.stats.rx_packets == rx_before);
        CHECK(g_broker.stats.rx_dropped > drop_before);
        {
            uint8_t byte = 0;
            CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch, TEST_PEER,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, &byte,
                  sizeof(byte), g_now) == 0);
            CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
                  epoch, generation, TEST_PEER, false) ==
                  CP_SESSION_ERR_STATE);
            cp_boot_ap_readback off = {0};
            CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &off,
                  g_now) == CP_SESSION_ERR_STATE);
        }
        cleanup();
        puts("PASS: real malformed CSM feed failure is immediate and gated");
        return 0;
    }

    if (strcmp(scenario, "ap_pre") == 0) {
        CHECK(cp_session_ap_state(&g_broker.session) == CP_BOOT_AP_READY);
        CHECK(g_broker.cfg.ap_readback.on_air);
        cleanup();
        puts("PASS: pre-session AP clock/validation keeps cfg and session coherent");
        return 0;
    }
    if (strcmp(scenario, "ap") == 0) {
        cp_boot_ap_readback off = {0};
        cp_boot_ap_readback on = {
            .on_air = true,
            .ssid = "SCOURGE-AP",
            .channel = 36u,
            .security = CP_WIRE_WIFI_WPA2,
            .link_local = "fe80::1"
        };
        cp_boot_ap_readback bad = on;
        bad.ssid = "";
        uint32_t epoch = cp_runtime_broker_epoch(&g_broker);
        uint32_t generation = cp_session_generation(&g_broker.session);
        for (unsigned i = 0; i < 200 &&
             !g_broker.session.boot.offer_active; i++)
            runtime_pump(1);
        CHECK(g_broker.session.boot.offer_active);
        CHECK(cp_session_ap_state(&g_broker.session) == CP_BOOT_AP_READY);
        /* Authenticated/awaiting: withdrawal must gate before activation. */
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &off, g_now) ==
              CP_SESSION_OK);
        CHECK(cp_session_ap_state(&g_broker.session) ==
              CP_BOOT_AP_NOT_READY);
        CHECK(!cp_session_receiver_active(&g_broker.session));
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on, g_now) ==
              CP_SESSION_OK);
        CHECK(cp_session_ap_state(&g_broker.session) == CP_BOOT_AP_READY);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
              epoch, generation, TEST_PEER, true) == CP_SESSION_OK);
        CHECK(cp_session_receiver_active(&g_broker.session));
        CHECK(cp_session_projection_active(&g_broker.session) == false);

        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &off, g_now) ==
              CP_SESSION_OK);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_session_ap_state(&g_broker.session) ==
              CP_BOOT_AP_NOT_READY);
        CHECK(!cp_session_receiver_active(&g_broker.session));
        CHECK(!cp_session_projection_active(&g_broker.session));

        cp_boot_ap_state before = cp_session_ap_state(&g_broker.session);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &bad, g_now) ==
              CP_SESSION_ERR_CONFIG);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_session_ap_state(&g_broker.session) == before);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on,
              g_now - 1u) == CP_SESSION_ERR_CLOCK_ROLLBACK);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on, g_now) ==
              CP_SESSION_OK);
        CHECK(g_broker.cfg.ap_readback.on_air);
        CHECK(cp_session_ap_state(&g_broker.session) == CP_BOOT_AP_READY);
        if (g_broker.session.boot.offer_active) {
            CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
                  epoch, generation, TEST_PEER, true) == CP_SESSION_OK);
            CHECK(cp_session_receiver_active(&g_broker.session));
            CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
                  epoch, generation, TEST_PEER, false) == CP_SESSION_OK);
        }

        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &off, g_now) ==
              CP_SESSION_OK);
        reset_phone();
        uint32_t old_epoch = cp_runtime_broker_epoch(&g_broker);
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        for (unsigned i = 0; i < 2000 && !g_broker.session_inited &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.session_inited);
        CHECK(!g_broker.cfg.ap_readback.on_air);
        CHECK(cp_session_ap_state(&g_broker.session) ==
              CP_BOOT_AP_NOT_READY);
        CHECK(!cp_session_receiver_active(&g_broker.session));
        CHECK(cp_runtime_broker_set_ap_readback(&g_broker, &on, g_now) ==
              CP_SESSION_OK);
        CHECK(g_broker.cfg.ap_readback.on_air);
        CHECK(cp_session_ap_state(&g_broker.session) == CP_BOOT_AP_READY);
        CHECK(wait_for_broker(scenario, 6000));
        CHECK(cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u);
        cleanup();
        puts("PASS: AP withdrawal gates active receiver and is not reused on restart");
        return 0;
    }

    if (strcmp(scenario, "epoch") == 0) {
        uint32_t epoch1 = cp_runtime_broker_epoch(&g_broker);
        uint32_t generation1 = cp_session_generation(&g_broker.session);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
              epoch1, generation1, TEST_PEER, false) == CP_SESSION_OK);
        reset_phone();
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        CHECK(wait_for_broker(scenario, 6000));
        uint32_t epoch2 = cp_runtime_broker_epoch(&g_broker);
        uint32_t generation2 = cp_session_generation(&g_broker.session);
        CHECK(epoch2 == epoch1 + 1u);
        CHECK(generation2 == generation1);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
              epoch1, generation2, TEST_PEER, false) ==
              CP_SESSION_ERR_GENERATION_MISMATCH);
        CHECK(cp_runtime_broker_set_receiver_active(&g_broker, g_now,
              epoch2, generation2, TEST_PEER, false) == CP_SESSION_OK);
        {
            uint8_t bogus[1] = {0};
            uint64_t before = g_broker.last_now_ms;
            CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch1, TEST_PEER,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, bogus,
                  sizeof(bogus), g_now + 1000u) == 0);
            CHECK(g_broker.last_now_ms == before);
            CHECK(cp_runtime_broker_feed_hook(&g_broker, epoch2, TEST_PEER2,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, bogus,
                  sizeof(bogus), g_now) == 0);
            runtime_pump(1);
            CHECK(g_broker.state == CP_RUNTIME_BROKER_ACTIVE);
            CHECK(!g_broker.fatal);
        }
        cleanup();
        puts("PASS: receiver/feed attestations are bound to the current runtime epoch");
        return 0;
    }
    if (strcmp(scenario, "overflow") == 0) {
        g_broker.epoch = UINT32_MAX;
        reset_phone();
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        for (unsigned i = 0; i < 2000 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.fatal);
        CHECK(g_broker.epoch == UINT32_MAX);
        CHECK(strstr(cp_runtime_broker_error(&g_broker), "epoch_exhausted") !=
              NULL);
        CHECK(g_broker.spp == NULL);
        cleanup();
        puts("PASS: runtime epoch exhaustion fails closed without wrap");
        return 0;
    }

    if (strcmp(scenario, "cancel") == 0) {
        uint32_t epoch = g_broker.epoch;
        CHECK(cp_runtime_broker_cancel(&g_broker, g_now) == CP_SESSION_OK);
        CHECK(cp_runtime_broker_epoch(&g_broker) == epoch);
        CHECK(g_broker.restart_required);
        CHECK(!g_broker.fatal);
        CHECK(g_broker.spp == NULL);
        CHECK(cp_runtime_broker_state(&g_broker) ==
              CP_RUNTIME_BROKER_BLOCKED);
        CHECK(cp_runtime_broker_tick(&g_broker, NULL, 0, g_now + 5) ==
              CP_SESSION_ERR_STATE);
        cleanup();
        puts("PASS: runtime broker cancellation requires an explicit new epoch");
        return 0;
    }
    if (strcmp(scenario, "usb") == 0 ||
        strcmp(scenario, "consent") == 0) {
        if (strcmp(scenario, "usb") == 0) g_usb_active = true;
        else g_consent = CP_BOOT_CONSENT_DENIED;
        {
            uint8_t byte = 0;
            CHECK(cp_runtime_broker_feed_hook(&g_broker,
                  cp_runtime_broker_epoch(&g_broker), TEST_PEER,
                  CP_SPP_RX_UUID_KIND_IAP2_ACCESSORY_CAFF, &byte,
                  sizeof(byte), g_now) == 0);
        }
        runtime_pump(5);
        CHECK(g_broker.policy_blocked);
        CHECK(g_broker.restart_required);
        CHECK(!g_broker.fatal);
        CHECK(g_broker.spp == NULL);
        CHECK(cp_runtime_broker_state(&g_broker) ==
              CP_RUNTIME_BROKER_BLOCKED);
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) ==
              CP_SESSION_ERR_POLICY);
        g_usb_active = false;
        g_consent = CP_BOOT_CONSENT_GRANTED;
        reset_phone();
        uint32_t old_epoch = g_broker.epoch;
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        CHECK(wait_for_broker(scenario, 4000));
        CHECK(cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u);
        cleanup();
        puts("PASS: runtime broker USB/consent preemption requires restart");
        return 0;
    }
    if (strcmp(scenario, "owner") == 0) {
        char command = 'O';
        CHECK(write(g_cmd[1], &command, 1) == 1);
        for (unsigned i = 0; i < 3000 &&
             g_broker.state != CP_RUNTIME_BROKER_FATAL; i++)
            runtime_pump(1);
        CHECK(g_broker.state == CP_RUNTIME_BROKER_FATAL);
        CHECK(g_broker.fatal);
        CHECK(g_broker.restart_required);
        CHECK(g_broker.spp == NULL);
        CHECK(cp_runtime_broker_epoch(&g_broker) >= 1u);
        cleanup();
        puts("PASS: runtime broker owner loss is fatal and non-restarting");
        return 0;
    }
    if (strcmp(scenario, "peer") == 0) {
        char command = 'P';
        CHECK(write(g_cmd[1], &command, 1) == 1);
        drain_mock_data();
        g_tolerate_link_errors = true;
        reset_phone();
        uint32_t old_epoch = g_broker.epoch;
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        for (unsigned i = 0; i < 4000; i++) {
            runtime_pump(1);
            if (cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u &&
                strcmp(cp_runtime_broker_peer(&g_broker), TEST_PEER2) == 0)
                break;
            if (g_broker.state == CP_RUNTIME_BROKER_FATAL)
                break;
        }
        CHECK(strcmp(cp_runtime_broker_peer(&g_broker), TEST_PEER2) == 0);
        CHECK(cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u);
        cleanup();
        puts("PASS: runtime broker drains stale RX and binds a new peer");
        return 0;
    }
    if (strcmp(scenario, "stale") == 0) {
        const uint8_t stale[] = "STALE-RX";
        send_frame(TEST_PEER, stale, sizeof(stale) - 1);
        drain_mock_data();
        reset_phone();
        uint32_t old_epoch = g_broker.epoch;
        CHECK(cp_runtime_broker_restart(&g_broker, g_now) == CP_SESSION_OK);
        CHECK(wait_for_broker(scenario, 4000));
        CHECK(cp_runtime_broker_epoch(&g_broker) == old_epoch + 1u);
        CHECK(g_broker.stats.rx_dropped > 0u);
        cleanup();
        puts("PASS: runtime broker drains stale hook RX before a new epoch");
        return 0;
    }

    cleanup();
    puts("PASS: runtime broker private-bus SPP, hook RX, helper auth and session");
    return 0;
}
