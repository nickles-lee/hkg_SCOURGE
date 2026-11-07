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
 * Anything else fails closed with UnknownMethod, except the lab-only vehicle
 * state interface below.
 *
 * Lab vehicle-state model (NOT a vendor DBC and not stock fidelity):
 *   Fixed-frame writes are packed little-endian into a per-canid 64-bit
 *   frame (startbit 0 = LSB of byte 0). Two lab-defined canids decode into a
 *   deterministic state:
 *
 *     0x0100  bytes 0..1  speed_kph (0..300, clamped)
 *             byte 2 bits 0..1  gear (0=P, 1=R, 2=N, 3=D)
 *             byte 2 bits 2..3  ignition (0=OFF, 1=ACC, 2=ON, 3=START)
 *     0x0101  byte 0  battery SOC percent (0..100, clamped)
 *             byte 1 bits 0..3  doors bitmask (FL, FR, RL, RR)
 *             byte 2  climate setpoint in half-degrees C (32..60 = 16..30,
 *                     clamped)
 *             byte 3  outside temperature as int8 C (-40..85, clamped)
 *
 *   Out-of-range values are clamped and logged:
 *     TWEAKS_VEHICLE_CLAMP field=speed requested=1000 applied=300
 *
 *   The last SetCANMicomRecv payload is retained as bulk=<hex>.
 *
 * Lab-only D-Bus interface on the same bus name (for deterministic tests):
 *   path /Vehicle, interface org.scourge.vehicle.State
 *     GetState() -> s
 *     Reset() -> ()
 *
 *   GetState also logs the full state to the simulator log:
 *     TWEAKS_VEHICLE_STATE speed=42 gear=3 ... frames=0100,0101 bulk=063f
 */
#include <dbus/dbus.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUS_NAME "com.lge.car.micom"
#define CAN_PATH "/Can"
#define CAN_INTERFACE "com.lge.car.micom.can"
#define VEHICLE_PATH "/Vehicle"
#define VEHICLE_INTERFACE "org.scourge.vehicle.State"
#define MAX_FRAMES 16
#define MAX_BULK 32

typedef struct {
    uint8_t id[4];
    int id_len;
    uint8_t bytes[8];
} can_frame_t;

typedef struct {
    int speed_kph;
    int gear;
    int ignition;
    int battery_soc;
    int doors;
    int climate_raw;
    int outside_c;
} vehicle_state_t;

static volatile sig_atomic_t keep_running = 1;
static can_frame_t frames[MAX_FRAMES];
static int frame_count;
static vehicle_state_t vehicle = {0, 0, 0, 0, 0, 44, 20};
static uint8_t bulk[MAX_BULK];
static int bulk_len;

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static void reset_state(void)
{
    vehicle_state_t fresh = {0, 0, 0, 0, 0, 44, 20};
    vehicle = fresh;
    frame_count = 0;
    bulk_len = 0;
    printf("TWEAKS_VEHICLE_RESET\n");
    fflush(stdout);
}

static int clamp_value(const char *field, int requested, int low, int high)
{
    int applied = requested;
    if (applied < low)
        applied = low;
    if (applied > high)
        applied = high;
    if (applied != requested)
        printf("TWEAKS_VEHICLE_CLAMP field=%s requested=%d applied=%d\n",
               field, requested, applied);
    return applied;
}

static can_frame_t *frame_for(const uint8_t *id, int id_len)
{
    int i;
    for (i = 0; i < frame_count; ++i) {
        if (frames[i].id_len == id_len &&
            memcmp(frames[i].id, id, (size_t)id_len) == 0)
            return &frames[i];
    }
    if (frame_count >= MAX_FRAMES) {
        printf("TWEAKS_VEHICLE_WRITE_IGNORED reason=frame-limit\n");
        return NULL;
    }
    memset(&frames[frame_count], 0, sizeof(frames[frame_count]));
    memcpy(frames[frame_count].id, id, (size_t)id_len);
    frames[frame_count].id_len = id_len;
    /* A new 0x0101 frame starts at the state defaults (22.0 C, 20 C) so a
     * partial battery/doors write does not transiently clamp zero. */
    if (id_len == 2 && id[0] == 0x01 && id[1] == 0x01) {
        frames[frame_count].bytes[2] = 44;
        frames[frame_count].bytes[3] = (uint8_t)(int8_t)20;
    }
    return &frames[frame_count++];
}

static void decode_frame(can_frame_t *frame)
{
    if (frame->id_len != 2)
        return;
    if (frame->id[0] == 0x01 && frame->id[1] == 0x00) {
        int speed = frame->bytes[0] | (frame->bytes[1] << 8);
        vehicle.speed_kph = clamp_value("speed", speed, 0, 300);
        frame->bytes[0] = (uint8_t)(vehicle.speed_kph & 0xff);
        frame->bytes[1] = (uint8_t)(vehicle.speed_kph >> 8);
        vehicle.gear = frame->bytes[2] & 0x03;
        vehicle.ignition = (frame->bytes[2] >> 2) & 0x03;
    } else if (frame->id[0] == 0x01 && frame->id[1] == 0x01) {
        vehicle.battery_soc = clamp_value("battery", frame->bytes[0], 0, 100);
        frame->bytes[0] = (uint8_t)vehicle.battery_soc;
        vehicle.doors = frame->bytes[1] & 0x0f;
        vehicle.climate_raw = clamp_value("climate", frame->bytes[2], 32, 60);
        frame->bytes[2] = (uint8_t)vehicle.climate_raw;
        vehicle.outside_c = clamp_value("outside", (int8_t)frame->bytes[3],
                                        -40, 85);
        frame->bytes[3] = (uint8_t)(int8_t)vehicle.outside_c;
    }
}

static void apply_fixed(const uint8_t *id, int id_len, int startbit,
                        int size, int32_t data)
{
    can_frame_t *frame;
    uint32_t value = (uint32_t)data;
    int i;

    if (id == NULL || id_len <= 0 || id_len > 4) {
        printf("TWEAKS_VEHICLE_WRITE_IGNORED reason=id\n");
        return;
    }
    /* Check startbit before arithmetic: an untrusted int32 near INT_MAX
     * would overflow startbit + size and bypass an overflow-prone check. */
    if (startbit < 0 || startbit > 63 || size <= 0 || size > 32 ||
        startbit > 64 - size) {
        printf("TWEAKS_VEHICLE_WRITE_IGNORED reason=range startbit=%d size=%d\n",
               startbit, size);
        return;
    }
    frame = frame_for(id, id_len);
    if (frame == NULL)
        return;
    for (i = 0; i < size; ++i) {
        int pos = startbit + i;
        int bit = (int)((value >> i) & 1u);
        if (bit)
            frame->bytes[pos / 8] |= (uint8_t)(1u << (pos % 8));
        else
            frame->bytes[pos / 8] &= (uint8_t)~(1u << (pos % 8));
    }
    decode_frame(frame);
}

static void format_state(char *out, size_t cap)
{
    size_t used;
    int i, j;

    used = (size_t)snprintf(out, cap,
                            "speed=%d gear=%d ignition=%d battery=%d doors=%d "
                            "climate=%d.%d outside=%d frames=",
                            vehicle.speed_kph, vehicle.gear, vehicle.ignition,
                            vehicle.battery_soc, vehicle.doors,
                            vehicle.climate_raw / 2,
                            (vehicle.climate_raw % 2) * 5, vehicle.outside_c);
    for (i = 0; i < frame_count && used < cap; ++i) {
        if (i > 0 && used < cap)
            used += (size_t)snprintf(out + used, cap - used, ",");
        for (j = 0; j < frames[i].id_len && used < cap; ++j)
            used += (size_t)snprintf(out + used, cap - used, "%02x",
                                     frames[i].id[j]);
    }
    if (used < cap)
        used += (size_t)snprintf(out + used, cap - used, " bulk=");
    for (i = 0; i < bulk_len && used < cap; ++i)
        used += (size_t)snprintf(out + used, cap - used, "%02x", bulk[i]);
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

static void send_string(DBusConnection *connection, DBusMessage *request,
                        const char *value)
{
    DBusMessage *reply = dbus_message_new_method_return(request);
    DBusMessageIter iter;
    const char *text = value;
    if (reply == NULL)
        return;
    dbus_message_iter_init_append(reply, &iter);
    dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &text);
    dbus_connection_send(connection, reply, NULL);
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
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

static void handle_fixed(DBusMessage *message)
{
    DBusMessageIter iter, sub;
    const uint8_t *id = NULL;
    int id_len = 0;
    dbus_int32_t startbit = 0, size = 0, data = 0;

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_recurse(&iter, &sub);
    dbus_message_iter_get_fixed_array(&sub, &id, &id_len);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &startbit);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &size);
    dbus_message_iter_next(&iter);
    dbus_message_iter_get_basic(&iter, &data);
    apply_fixed(id, id_len, startbit, size, data);
}

static void handle_bulk(DBusMessage *message)
{
    DBusMessageIter iter, sub;
    const uint8_t *data = NULL;
    int len = 0, i;

    dbus_message_iter_init(message, &iter);
    dbus_message_iter_recurse(&iter, &sub);
    dbus_message_iter_get_fixed_array(&sub, &data, &len);
    if (data != NULL) {
        bulk_len = len > MAX_BULK ? MAX_BULK : len;
        memcpy(bulk, data, (size_t)bulk_len);
    } else {
        bulk_len = 0;
    }
    printf("TWEAKS_VEHICLE_BULK length=%d retained=%d hex=", len, bulk_len);
    for (i = 0; i < bulk_len; ++i)
        printf("%02x", bulk[i]);
    printf("\n");
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
                    handle_fixed(request);
                    printf("TWEAKS_CAN_SIM_ACK member=SetCANFixedMicomRecv\n");
                    fflush(stdout);
                    send_empty(connection, request);
                    handled = 1;
                } else if (path != NULL && strcmp(path, CAN_PATH) == 0 &&
                           dbus_message_is_method_call(request, CAN_INTERFACE, "SetCANMicomRecv") &&
                           strcmp(signature, "ay") == 0) {
                    log_arguments(request);
                    handle_bulk(request);
                    printf("TWEAKS_CAN_SIM_ACK member=SetCANMicomRecv\n");
                    fflush(stdout);
                    send_empty(connection, request);
                    handled = 1;
                } else if (path != NULL && strcmp(path, VEHICLE_PATH) == 0 &&
                           dbus_message_is_method_call(request, VEHICLE_INTERFACE, "GetState") &&
                           strcmp(signature, "") == 0) {
                    char state[1024];
                    format_state(state, sizeof(state));
                    printf("TWEAKS_VEHICLE_STATE %s\n", state);
                    fflush(stdout);
                    send_string(connection, request, state);
                    handled = 1;
                } else if (path != NULL && strcmp(path, VEHICLE_PATH) == 0 &&
                           dbus_message_is_method_call(request, VEHICLE_INTERFACE, "Reset") &&
                           strcmp(signature, "") == 0) {
                    reset_state();
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
