/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * cp_spp_dbus — bounded, private-bus CommonAPI SPP transport.
 *
 * OFFLINE, UNACTIVATED IMPLEMENTATION.  It opens only the explicit Unix
 * private/session bus address supplied by the caller, never a system bus and
 * never an autolaunch address.  It sends no Hello until cp_spp_dbus_start(),
 * activates no service and changes no global radio state.  The caller owns the
 * event loop and must call cp_spp_dbus_tick() with monotonic milliseconds.
 *
 * The method argument shapes below are validated against the service's own
 * introspection XML before they are used.  This module deliberately does not
 * require the pinned (service name, accessory UUID) -> int32 signature for
 * both ListenSPP and StopListenSPP; any other shape is fatal.
 * SendData must be exactly (ay) -> i.  A successful int32 0 is only the OEM
 * stub ACK, never proof of delivery.  Inbound data is accepted only from a
 * credential-checked first-party hook through cp_spp_dbus_deliver_rx(), only
 * for the CAFF accessory kind and only for one bound peer address.
 *
 * XML parsing: libxml2 is used when compiled with CP_SPP_DBUS_HAVE_LIBXML2.
 * Without it, introspection fails closed instead of using a home-grown parser
 * or vendoring a third-party implementation.  DTD/entity declarations are
 * rejected before parsing and external network access is disabled.
 */
#include "cp_spp_dbus.h"

#include <dbus/dbus.h>
#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(CP_SPP_DBUS_HAVE_LIBXML2)
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

#define CP_SPP_DBUS_MAX_ADDRESS 512u
#define CP_SPP_DBUS_MAX_PATH 256u
#define CP_SPP_DBUS_MAX_IFACE 256u
#define CP_SPP_DBUS_MAX_SERVICE 256u
#define CP_SPP_DBUS_MAX_UNIQUE 256u
#define CP_SPP_DBUS_MAX_ERROR 192u
#define CP_SPP_DBUS_MAX_SERIALS 3u
#define CP_SPP_DBUS_MAX_REPLIES 8u
#define CP_SPP_DBUS_MAX_START_DEADLINE 60000u
#define CP_SPP_DBUS_MAX_SEND_DEADLINE 10000u
#define CP_SPP_DBUS_NODE_BUDGET 4096u

typedef enum {
    CP_SPP_DBUS_PK_NONE = 0,
    CP_SPP_DBUS_PK_HELLO,
    CP_SPP_DBUS_PK_MATCH_STATUS,
    CP_SPP_DBUS_PK_MATCH_OWNER,
    CP_SPP_DBUS_PK_LIST_NAMES,
    CP_SPP_DBUS_PK_OWNER_ADAPTER,
    CP_SPP_DBUS_PK_OWNER_SERIAL,
    CP_SPP_DBUS_PK_OWNER_CONNECTED,
    CP_SPP_DBUS_PK_UID,
    CP_SPP_DBUS_PK_XML_ADAPTER,
    CP_SPP_DBUS_PK_LISTEN,
    CP_SPP_DBUS_PK_STOP,
    CP_SPP_DBUS_PK_XML_SERIAL,
    CP_SPP_DBUS_PK_SEND
} cp_spp_dbus_pk;

typedef struct {
    bool used;
    cp_spp_dbus_pk kind;
    uint32_t serial;
    uint64_t deadline_ms;
    char aux[CP_SPP_DBUS_MAX_SERVICE];
} cp_spp_dbus_pending;

typedef struct {
    cp_spp_dbus_pk kind;
    bool error;
    int32_t ival;
    uint32_t uval;
    DBusMessage *xml;
    char sval[CP_SPP_DBUS_MAX_UNIQUE];
    char aux[CP_SPP_DBUS_MAX_SERVICE];
    char eval[64];
} cp_spp_dbus_reply;

typedef struct {
    uint16_t len;
    uint8_t bytes[CP_SPP_DBUS_MAX_TX_BYTES];
} cp_spp_dbus_frame;

struct cp_spp_dbus {
    DBusConnection *conn;
    int fd;
    bool opened;
    bool experimental;
    bool adapter_validated;
    bool listener_owned;
    bool listen_pending;
    bool stop_pending;
    bool connected;
    bool serial_pending;
    bool send_inflight;
    bool fatal_notified;
    bool owner_lost;
    bool status_pending;
    bool list_invalid;
    bool list_adapter_found;
    bool attach_pending;
    bool attached;
    uint8_t list_serial_count;
    uint8_t listen_args;
    uint8_t stop_args;
    uint8_t match_done;
    uint8_t owner_wait;
    uint32_t connect_generation;
    cp_spp_dbus_state state;
    uint64_t last_now_ms;
    uint64_t validation_deadline_ms;
    uint64_t send_deadline_ms;
    uint32_t send_serial;
    uint32_t ignored_send_serial;
    uint32_t inflight_len;
    uint32_t expected_owner_uid;
    uint32_t control_timeout_ms;
    uint32_t send_timeout_ms;
    size_t max_tx_bytes;
    cp_spp_dbus_callbacks cb;
    void *user;

    char bus_address[CP_SPP_DBUS_MAX_ADDRESS];
    char adapter_service[CP_SPP_DBUS_MAX_SERVICE];
    char adapter_path[CP_SPP_DBUS_MAX_PATH];
    char adapter_interface[CP_SPP_DBUS_MAX_IFACE];
    char serial_prefix[CP_SPP_DBUS_MAX_SERVICE];
    char serial_interface[CP_SPP_DBUS_MAX_IFACE];
    char uuid_accessory[CP_SPP_DBUS_MAX_UUID];
    char uuid_phone[CP_SPP_DBUS_MAX_UUID];
    char service_name[CP_SPP_DBUS_MAX_SERVICE];
    char unique_owner[CP_SPP_DBUS_MAX_UNIQUE];
    char connected_path[CP_SPP_DBUS_MAX_PATH];
    char pending_path[CP_SPP_DBUS_MAX_PATH];
    char status_path[CP_SPP_DBUS_MAX_PATH];
    char status_value[CP_SPP_DBUS_MAX_VALUE];
    char attach_path[CP_SPP_DBUS_MAX_PATH];
    char attach_service[CP_SPP_DBUS_MAX_SERVICE];
    char status_signature[3];
    char serial_status_signature[3];
    char rx_peer[18];
    char connected_service[CP_SPP_DBUS_MAX_SERVICE];
    char error[CP_SPP_DBUS_MAX_ERROR];

    cp_spp_dbus_pending pending[CP_SPP_DBUS_MAX_PENDING];
    cp_spp_dbus_reply replies[CP_SPP_DBUS_MAX_REPLIES];
    uint8_t reply_count;
    cp_spp_dbus_frame tx[CP_SPP_DBUS_MAX_TX_QUEUE];
    uint8_t tx_head;
    uint8_t tx_count;

    char list_serial[CP_SPP_DBUS_MAX_SERIALS][CP_SPP_DBUS_MAX_SERVICE];
    char serial_owner[CP_SPP_DBUS_MAX_SERIALS][CP_SPP_DBUS_MAX_UNIQUE];
};

_Static_assert(sizeof(struct cp_spp_dbus) <= CP_SPP_DBUS_STORAGE_BYTES,
               "cp_spp_dbus must fit caller storage");
_Static_assert(_Alignof(struct cp_spp_dbus) <= _Alignof(max_align_t),
               "cp_spp_dbus alignment");

static bool bounded_name(const char *s, size_t max)
{
    size_t n;
    if (s == NULL || s[0] == '\0') return false;
    n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
            return false;
    }
    return true;
}

static bool bounded_path(const char *s, size_t max)
{
    size_t n;
    if (s == NULL || s[0] != '/') return false;
    n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '/'))
            return false;
        if (c == '/' && i > 0 && s[i - 1] == '/') return false;
    }
    return true;
}

static bool bounded_text(const char *s, size_t max)
{
    size_t n;
    if (s == NULL) return false;
    n = strlen(s);
    if (n > max) return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e) return false;
    }
    return true;
}

static bool bounded_uuid(const char *s)
{
    size_t n;
    if (s == NULL) return false;
    n = strlen(s);
    if (n != 36u) return false;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8u || i == 13u || i == 18u || i == 23u) {
            if (c != '-') return false;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool bounded_bus_address(const char *s)
{
    size_t n;
    if (s == NULL || strncmp(s, "unix:", 5u) != 0) return false;
    n = strlen(s);
    if (n < 10u || n >= CP_SPP_DBUS_MAX_ADDRESS) return false;
    if (strstr(s, ";") != NULL || strstr(s, "autolaunch:") != NULL)
        return false;
    if (strstr(s, "path=") == NULL && strstr(s, "abstract=") == NULL)
        return false;
    for (size_t i = 5u; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c > 0x7e || c == ' ' || c == '\t')
            return false;
    }
    return true;
}

static bool bounded_mac(const char *s)
{
    if (s == NULL || strlen(s) != 17u) return false;
    for (size_t i = 0; i < 17u; i++) {
        if (i % 3u == 2u) {
            if (s[i] != ':') return false;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f') ||
                     (s[i] >= 'A' && s[i] <= 'F'))) {
            return false;
        }
    }
    return true;
}

static bool mac_equal_ci(const char *a, const char *b)
{
    for (size_t i = 0; i < 17u; i++) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'F') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'F') y = (char)(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

/* The pinned CommonAPI object and name encode the same peer and CAFF UUID.
 * Neither a generic object path nor the first hook datagram can select a peer. */
static bool serial_identity(cp_spp_dbus *engine, const char *path,
                            char peer[18], char service[CP_SPP_DBUS_MAX_SERVICE])
{
    if (path == NULL || strlen(path) != 66u ||
        strncmp(path, "/dev_", 5u) || strncmp(path + 22u, "/serial_", 8u))
        return false;
    for (size_t i = 0; i < 17u; i++)
        peer[i] = path[5u + i] == '_' ? ':' : path[5u + i];
    peer[17] = '\0';
    if (!bounded_mac(peer)) return false;
    for (size_t i = 0; i < 36u; i++) {
        char actual = path[30u + i], expected = engine->uuid_accessory[i];
        if (expected == '-') expected = '_';
        if (actual >= 'a' && actual <= 'f') actual -= 'a' - 'A';
        if (expected >= 'a' && expected <= 'f') expected -= 'a' - 'A';
        if (actual != expected) return false;
    }
    int n = snprintf(service, CP_SPP_DBUS_MAX_SERVICE, "%s%s",
                     engine->serial_prefix, path + 1u);
    if (n < 0 || n >= (int)CP_SPP_DBUS_MAX_SERVICE) return false;
    for (char *p = service; *p; p++) if (*p == '/') *p = '.';
    return bounded_name(service, CP_SPP_DBUS_MAX_SERVICE - 1u);
}

/* Reverse of the well-known name derivation: the registered SerialPort
 * service name carries the same peer and CAFF UUID as its object path.  The
 * object is registered by SetSerialPortConnectionStatus(..., CONNECTED), so a
 * well-known name present at validation is the only attach-time evidence the
 * bus exposes.  It is still revalidated by serial_identity() and by the
 * service owner before use. */
static bool copy_bounded(char *dst, size_t cap, const char *src);
static void fatal(cp_spp_dbus *engine, const char *fmt, ...);

static bool serial_path_from_service(cp_spp_dbus *engine, const char *service,
                                     char *path, size_t cap)
{
    size_t prefix_len = strlen(engine->serial_prefix);
    size_t n;
    if (service == NULL || strncmp(service, engine->serial_prefix,
                                   prefix_len) != 0)
        return false;
    const char *suffix = service + prefix_len;
    n = strlen(suffix);
    if (n == 0u || n + 2u > cap) return false;
    path[0] = '/';
    for (size_t i = 0; i < n; i++) {
        char c = suffix[i];
        path[i + 1u] = c == '.' ? '/' : c;
    }
    path[n + 1u] = '\0';
    return bounded_path(path, cap - 1u);
}

static void select_attach_candidate(cp_spp_dbus *engine)
{
    char peer[18];
    char service[CP_SPP_DBUS_MAX_SERVICE];
    char path[CP_SPP_DBUS_MAX_PATH];
    unsigned candidates = 0;
    engine->attach_pending = false;
    engine->attach_path[0] = '\0';
    engine->attach_service[0] = '\0';
    for (uint8_t i = 0; i < engine->list_serial_count; i++) {
        if (strcmp(engine->serial_owner[i], engine->unique_owner) != 0)
            continue;
        if (!serial_path_from_service(engine, engine->list_serial[i], path,
                                      sizeof(path)))
            continue;
        if (!serial_identity(engine, path, peer, service))
            continue;
        if (strcmp(service, engine->list_serial[i]) != 0) continue;
        candidates++;
        if (!copy_bounded(engine->attach_path, sizeof(engine->attach_path),
                          path) ||
            !copy_bounded(engine->attach_service,
                          sizeof(engine->attach_service), service)) {
            fatal(engine, "attach_path_too_long");
            return;
        }
    }
    if (candidates > 1u) {
        /* More than one registered SerialPort name is not itself proof that
         * all are connected; wait for an explicit status signal instead of
         * guessing which object the phone is using. */
        engine->attach_pending = false;
        return;
    }
    engine->attach_pending = candidates == 1u;
}

static bool copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t n;
    if (src == NULL) return false;
    n = strlen(src);
    if (n >= cap) return false;
    memcpy(dst, src, n + 1u);
    return true;
}

static uint64_t deadline_add(uint64_t now, uint32_t timeout)
{
    if (now > UINT64_MAX - (uint64_t)timeout) return UINT64_MAX;
    return now + (uint64_t)timeout;
}

static cp_spp_dbus_state set_state(cp_spp_dbus *engine,
                                   cp_spp_dbus_state state)
{
    cp_spp_dbus_state old = engine->state;
    if (old == state) return old;
    engine->state = state;
    if (engine->cb.on_state)
        engine->cb.on_state(engine->user, old, state);
    return old;
}

static void fatal(cp_spp_dbus *engine, const char *fmt, ...)
{
    va_list ap;
    if (engine == NULL || engine->state == CP_SPP_DBUS_STATE_FATAL) return;
    va_start(ap, fmt);
    vsnprintf(engine->error, sizeof(engine->error), fmt, ap);
    va_end(ap);
    engine->state = CP_SPP_DBUS_STATE_FATAL;
    engine->listen_pending = false;
    engine->stop_pending = false;
    engine->send_inflight = false;
    engine->serial_pending = false;
    engine->tx_count = 0;
    engine->tx_head = 0;
    memset(engine->pending, 0, sizeof(engine->pending));
    for (uint8_t i = 0; i < engine->reply_count; i++) {
        if (engine->replies[i].xml != NULL) {
            dbus_message_unref(engine->replies[i].xml);
            engine->replies[i].xml = NULL;
        }
    }
    engine->reply_count = 0;
    if (!engine->fatal_notified) {
        engine->fatal_notified = true;
        if (engine->cb.on_fatal)
            engine->cb.on_fatal(engine->user, engine->error);
    }
}

static cp_spp_dbus_pending *pending_find_serial(cp_spp_dbus *engine,
                                                uint32_t serial)
{
    for (size_t i = 0; i < CP_SPP_DBUS_MAX_PENDING; i++) {
        if (engine->pending[i].used &&
            engine->pending[i].serial == serial)
            return &engine->pending[i];
    }
    return NULL;
}

static cp_spp_dbus_pending *pending_alloc(cp_spp_dbus *engine)
{
    for (size_t i = 0; i < CP_SPP_DBUS_MAX_PENDING; i++) {
        if (!engine->pending[i].used) return &engine->pending[i];
    }
    return NULL;
}

static bool reply_push(cp_spp_dbus *engine, cp_spp_dbus_pk kind,
                       bool error, const char *aux)
{
    cp_spp_dbus_reply *reply;
    if (engine->reply_count >= CP_SPP_DBUS_MAX_REPLIES) {
        fatal(engine, "reply_queue_overflow");
        return false;
    }
    reply = &engine->replies[engine->reply_count++];
    memset(reply, 0, sizeof(*reply));
    reply->kind = kind;
    reply->error = error;
    if (aux != NULL) (void)copy_bounded(reply->aux, sizeof(reply->aux), aux);
    return true;
}

static bool send_message(cp_spp_dbus *engine, DBusMessage *message,
                         cp_spp_dbus_pk kind, uint64_t now_ms,
                         const char *aux)
{
    dbus_uint32_t serial = 0;
    cp_spp_dbus_pending *pending = NULL;
    if (message == NULL) return false;
    dbus_message_set_auto_start(message, FALSE);
    if (kind != CP_SPP_DBUS_PK_NONE) {
        pending = pending_alloc(engine);
        if (pending == NULL) {
            dbus_message_unref(message);
            fatal(engine, "pending_overflow");
            return false;
        }
    }
    if (!dbus_connection_send(engine->conn, message, &serial) || serial == 0) {
        dbus_message_unref(message);
        fatal(engine, "send_failed");
        return false;
    }
    if (pending != NULL) {
        pending->used = true;
        pending->kind = kind;
        pending->serial = serial;
        pending->deadline_ms = deadline_add(now_ms, engine->control_timeout_ms);
        if (aux != NULL)
            (void)copy_bounded(pending->aux, sizeof(pending->aux), aux);
    }
    dbus_message_unref(message);
    return true;
}

static bool send_noarg_call(cp_spp_dbus *engine, const char *destination,
                            const char *path, const char *interface,
                            const char *method, cp_spp_dbus_pk kind,
                            uint64_t now_ms, const char *aux)
{
    DBusMessage *message = dbus_message_new_method_call(
        destination, path, interface, method);
    return send_message(engine, message, kind, now_ms, aux);
}

static bool send_string_call(cp_spp_dbus *engine, const char *destination,
                             const char *path, const char *interface,
                             const char *method, const char *value,
                             const char *value2, cp_spp_dbus_pk kind,
                             uint64_t now_ms, const char *aux)
{
    DBusMessage *message = dbus_message_new_method_call(
        destination, path, interface, method);
    DBusMessageIter iter;
    if (message == NULL) return false;
    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value) ||
        (value2 != NULL &&
         !dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &value2))) {
        dbus_message_unref(message);
        fatal(engine, "message_build_failed");
        return false;
    }
    return send_message(engine, message, kind, now_ms, aux);
}

static bool signature_is(DBusMessage *message, const char *signature)
{
    const char *actual = dbus_message_get_signature(message);
    return actual != NULL && strcmp(actual, signature) == 0;
}

/* ---------------------------------------------------------------- */
/* bounded introspection XML                                         */
/* ---------------------------------------------------------------- */

#if defined(CP_SPP_DBUS_HAVE_LIBXML2)
static bool ci_contains(const char *text, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return false;
    for (size_t i = 0; i + nlen <= len; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = text[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            j++;
        }
        if (j == nlen) return true;
    }
    return false;
}

static bool xml_attr_bounded(xmlNodePtr node, const char *name,
                             char *out, size_t cap)
{
    xmlChar *value = xmlGetProp(node, BAD_CAST name);
    bool ok = false;
    if (value != NULL) {
        size_t length = strlen((const char *)value);
        if (length < cap) {
            memcpy(out, value, length + 1u);
            ok = true;
        }
        xmlFree(value);
    }
    return ok;
}

static bool xml_name_is(xmlNodePtr node, const char *name)
{
    return node->type == XML_ELEMENT_NODE &&
           xmlStrcmp(node->name, BAD_CAST name) == 0;
}

/* Collect method argument types in declaration order.  Returns false on
 * malformed/overflowing argument lists. */
static bool xml_arg_types(xmlNodePtr method, bool signal,
                          char *in_types, size_t in_cap,
                          char *out_types, size_t out_cap)
{
    size_t in_used = 0, out_used = 0;
    unsigned nodes = 0;
    for (xmlNodePtr child = method->children; child; child = child->next) {
        if (!xml_name_is(child, "arg")) continue;
        if (++nodes > 32u) return false;
        char type[16] = "";
        char direction[8] = "";
        if (!xml_attr_bounded(child, "type", type, sizeof(type)))
            return false;
        (void)xml_attr_bounded(child, "direction", direction, sizeof(direction));
        bool is_out = signal;
        if (direction[0] != '\0') {
            if (strcmp(direction, "in") == 0) is_out = false;
            else if (strcmp(direction, "out") == 0) is_out = true;
            else return false;
        }
        char *dst = is_out ? out_types : in_types;
        size_t *used = is_out ? &out_used : &in_used;
        size_t cap = is_out ? out_cap : in_cap;
        size_t tlen = strlen(type);
        if (tlen == 0 || *used + tlen + 1u > cap) return false;
        memcpy(dst + *used, type, tlen);
        *used += tlen;
        dst[*used] = '\0';
    }
    return true;
}
#endif

static bool introspect_method(const char *xml, size_t length,
                              const char *interface, const char *method,
                              char *in_types, size_t in_cap,
                              char *out_types, size_t out_cap, bool signal)
{
#if defined(CP_SPP_DBUS_HAVE_LIBXML2)
    bool found = false;
    bool duplicate = false;
    unsigned nodes = 0;
    if (xml == NULL || length == 0 || length > CP_SPP_DBUS_MAX_XML_BYTES)
        return false;
    if (strlen(xml) != length) return false;
    if (ci_contains(xml, length, "<!DOCTYPE") ||
        ci_contains(xml, length, "<!ENTITY"))
        return false;
    xmlDocPtr doc = xmlReadMemory(xml, (int)length, "introspection.xml", NULL,
        XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING |
        XML_PARSE_COMPACT);
    if (doc == NULL) return false;
    xmlNodePtr root = xmlDocGetRootElement(doc);
    if (root == NULL || !xml_name_is(root, "node")) {
        xmlFreeDoc(doc);
        return false;
    }
    for (xmlNodePtr iface = root->children; iface; iface = iface->next) {
        if (!xml_name_is(iface, "interface")) continue;
        if (++nodes > CP_SPP_DBUS_NODE_BUDGET) {
            xmlFreeDoc(doc);
            return false;
        }
        char name[CP_SPP_DBUS_MAX_IFACE];
        if (!xml_attr_bounded(iface, "name", name, sizeof(name))) continue;
        if (strcmp(name, interface) != 0) continue;
        for (xmlNodePtr item = iface->children; item; item = item->next) {
            if (!xml_name_is(item, signal ? "signal" : "method")) continue;
            if (++nodes > CP_SPP_DBUS_NODE_BUDGET) {
                xmlFreeDoc(doc);
                return false;
            }
            char item_name[CP_SPP_DBUS_MAX_SERVICE];
            if (!xml_attr_bounded(item, "name", item_name,
                                  sizeof(item_name)))
                continue;
            if (strcmp(item_name, method) != 0) continue;
            if (found) { duplicate = true; break; }
            found = true;
            if (!xml_arg_types(item, signal, in_types, in_cap,
                               out_types, out_cap)) {
                xmlFreeDoc(doc);
                return false;
            }
        }
        if (duplicate) break;
    }
    xmlFreeDoc(doc);
    return found && !duplicate;
#else
    (void)xml;
    (void)length;
    (void)interface;
    (void)method;
    (void)in_types;
    (void)in_cap;
    (void)out_types;
    (void)out_cap;
    (void)signal;
    return false;
#endif
}

static bool introspect_listen(const char *xml, size_t length,
                              const char *interface, uint8_t *args_out)
{
    char ins[32] = "", outs[16] = "";
    if (!introspect_method(xml, length, interface,
                           "ListenSPP", ins, sizeof(ins), outs, sizeof(outs), false))
        return false;
    if (strcmp(outs, "i") != 0) return false;
    if (strcmp(ins, "ss") == 0) {
        *args_out = 2;
        return true;
    }
    return false;
}

static bool introspect_stop(const char *xml, size_t length, const char *interface,
                            uint8_t *args_out)
{
    char ins[16] = "", outs[16] = "";
    if (!introspect_method(xml, length, interface,
                            "StopListenSPP", ins, sizeof(ins), outs,
                            sizeof(outs), false))
        return false;
    if (strcmp(outs, "i") != 0) return false;
    if (strcmp(ins, "ss") == 0) {
        *args_out = 2;
        return true;
    }
    if (strcmp(ins, "s") == 0) {
        *args_out = 1;
        return true;
    }
    return false;
}

static bool introspect_send(const char *xml, size_t length, const char *interface)
{
    char ins[16] = "", outs[16] = "";
    if (!introspect_method(xml, length, interface,
                            CP_SPP_DBUS_SERIAL_METHOD_SEND_DATA,
                            ins, sizeof(ins), outs, sizeof(outs), false))
        return false;
    return strcmp(ins, "ay") == 0 && strcmp(outs, "i") == 0;
}

/* ---------------------------------------------------------------- */
/* incoming replies and signals                                      */
/* ---------------------------------------------------------------- */

static bool driver_sender(DBusMessage *message)
{
    const char *sender = dbus_message_get_sender(message);
    return sender != NULL && strcmp(sender, "org.freedesktop.DBus") == 0;
}

static bool service_sender(cp_spp_dbus *engine, DBusMessage *message)
{
    const char *sender = dbus_message_get_sender(message);
    return sender != NULL && engine->unique_owner[0] != '\0' &&
           strcmp(sender, engine->unique_owner) == 0;
}

static void parse_list_names(cp_spp_dbus *engine, DBusMessage *message)
{
    DBusMessageIter iter, array;
    engine->list_invalid = false;
    engine->list_adapter_found = false;
    engine->list_serial_count = 0;
    if (!signature_is(message, "as") ||
        !dbus_message_iter_init(message, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY) {
        engine->list_invalid = true;
        return;
    }
    dbus_message_iter_recurse(&iter, &array);
    unsigned count = 0;
    while (dbus_message_iter_get_arg_type(&array) == DBUS_TYPE_STRING) {
        const char *name = NULL;
        dbus_message_iter_get_basic(&array, &name);
        if (name == NULL || strlen(name) >= CP_SPP_DBUS_MAX_SERVICE) {
            engine->list_invalid = true;
            return;
        }
        if (++count > 512u) {
            engine->list_invalid = true;
            return;
        }
        /* Unique names (:1.2) and unrelated services are irrelevant; only
         * the adapter and serial well-known names are retained. */
        if (strcmp(name, engine->adapter_service) == 0) {
            if (!bounded_name(name, CP_SPP_DBUS_MAX_SERVICE - 1u)) {
                engine->list_invalid = true;
                return;
            }
            engine->list_adapter_found = true;
        } else if (strncmp(name, engine->serial_prefix,
                           strlen(engine->serial_prefix)) == 0) {
            if (!bounded_name(name, CP_SPP_DBUS_MAX_SERVICE - 1u) ||
                engine->list_serial_count >= CP_SPP_DBUS_MAX_SERIALS) {
                engine->list_invalid = true;
                return;
            }
            (void)copy_bounded(
                engine->list_serial[engine->list_serial_count],
                sizeof(engine->list_serial[0]), name);
            engine->list_serial_count++;
        }
        dbus_message_iter_next(&array);
    }
    if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID)
        engine->list_invalid = true;
}

static void handle_reply(cp_spp_dbus *engine, DBusMessage *message)
{
    if (engine->state == CP_SPP_DBUS_STATE_FATAL) return;
    uint32_t serial = dbus_message_get_reply_serial(message);
    cp_spp_dbus_pending *pending = NULL;
    cp_spp_dbus_pk kind = CP_SPP_DBUS_PK_NONE;
    bool is_error = dbus_message_get_type(message) ==
                    DBUS_MESSAGE_TYPE_ERROR;
    bool is_send = engine->send_inflight && serial == engine->send_serial;
    if (!is_send && engine->ignored_send_serial != 0u &&
        serial == engine->ignored_send_serial) {
        /* A disconnect already retired this SendData.  Its late ACK/error is
         * consumed as stale, never as delivery or a new protocol error. */
        engine->ignored_send_serial = 0u;
        return;
    }
    if (is_send) {
        kind = CP_SPP_DBUS_PK_SEND;
    } else {
        pending = pending_find_serial(engine, serial);
        if (pending == NULL) {
            fatal(engine, "unexpected_reply_%u", serial);
            return;
        }
        kind = pending->kind;
    }
    bool driver = kind == CP_SPP_DBUS_PK_HELLO ||
                  kind == CP_SPP_DBUS_PK_MATCH_STATUS ||
                  kind == CP_SPP_DBUS_PK_MATCH_OWNER ||
                  kind == CP_SPP_DBUS_PK_LIST_NAMES ||
                  kind == CP_SPP_DBUS_PK_OWNER_ADAPTER ||
                  kind == CP_SPP_DBUS_PK_OWNER_SERIAL ||
                  kind == CP_SPP_DBUS_PK_OWNER_CONNECTED ||
                  kind == CP_SPP_DBUS_PK_UID;
    if (driver ? !driver_sender(message) : !service_sender(engine, message)) {
        fatal(engine, "unexpected_reply_owner");
        return;
    }
    if (is_error) {
        const char *name = dbus_message_get_error_name(message);
        if (kind == CP_SPP_DBUS_PK_SEND) {
            engine->send_inflight = false;
            fatal(engine, "send_error_%s", name ? name : "unknown");
            return;
        }
        if (!reply_push(engine, kind, true,
                        pending != NULL ? pending->aux : NULL)) return;
        if (engine->reply_count > 0) {
            cp_spp_dbus_reply *reply =
                &engine->replies[engine->reply_count - 1u];
            (void)copy_bounded(reply->eval, sizeof(reply->eval),
                               name ? name : "error");
        }
        pending->used = false;
        return;
    }
    if (kind == CP_SPP_DBUS_PK_HELLO) {
        const char *unique = NULL;
        if (!signature_is(message, "s") ||
            !dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_STRING, &unique,
                                   DBUS_TYPE_INVALID) ||
            unique == NULL || unique[0] != ':' ||
            !copy_bounded(engine->unique_owner,
                          sizeof(engine->unique_owner), unique)) {
            fatal(engine, "hello_malformed");
            pending->used = false;
            return;
        }
        reply_push(engine, kind, false, NULL);
    } else if (kind == CP_SPP_DBUS_PK_MATCH_STATUS ||
               kind == CP_SPP_DBUS_PK_MATCH_OWNER) {
        if (!signature_is(message, "")) {
            fatal(engine, "match_malformed");
        } else {
            reply_push(engine, kind, false, NULL);
        }
    } else if (kind == CP_SPP_DBUS_PK_LIST_NAMES) {
        parse_list_names(engine, message);
        reply_push(engine, kind, false, NULL);
    } else if (kind == CP_SPP_DBUS_PK_OWNER_ADAPTER ||
               kind == CP_SPP_DBUS_PK_OWNER_SERIAL ||
               kind == CP_SPP_DBUS_PK_OWNER_CONNECTED) {
        const char *owner = NULL;
        char owner_copy[CP_SPP_DBUS_MAX_UNIQUE];
        if (!signature_is(message, "s") ||
            !dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_STRING, &owner,
                                   DBUS_TYPE_INVALID) ||
            owner == NULL || owner[0] != ':' ||
            !copy_bounded(owner_copy, sizeof(owner_copy), owner)) {
            fatal(engine, "owner_malformed");
        } else {
            if (!reply_push(engine, kind, false, pending->aux)) return;
            if (engine->reply_count > 0)
                (void)copy_bounded(
                    engine->replies[engine->reply_count - 1u].sval,
                    sizeof(engine->replies[0].sval), owner_copy);
        }
    } else if (kind == CP_SPP_DBUS_PK_UID) {
        dbus_uint32_t uid = 0;
        if (!signature_is(message, "u") ||
            !dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_UINT32, &uid,
                                   DBUS_TYPE_INVALID)) {
            fatal(engine, "uid_malformed");
        } else {
            if (!reply_push(engine, kind, false, NULL)) return;
            engine->replies[engine->reply_count - 1u].uval = uid;
        }
    } else if (kind == CP_SPP_DBUS_PK_XML_ADAPTER ||
               kind == CP_SPP_DBUS_PK_XML_SERIAL) {
        if (!signature_is(message, "s")) {
            fatal(engine, "xml_reply_signature");
        } else {
            if (!reply_push(engine, kind, false, pending->aux)) return;
            engine->replies[engine->reply_count - 1u].xml =
                dbus_message_ref(message);
        }
    } else if (kind == CP_SPP_DBUS_PK_LISTEN ||
               kind == CP_SPP_DBUS_PK_STOP ||
               kind == CP_SPP_DBUS_PK_SEND) {
        dbus_int32_t value = 0;
        if (!signature_is(message, "i") ||
            !dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_INT32, &value,
                                   DBUS_TYPE_INVALID)) {
            fatal(engine, "status_reply_malformed");
        } else {
            if (kind == CP_SPP_DBUS_PK_SEND) {
                engine->send_inflight = false;
                if (value != 0) {
                    fatal(engine, "send_rejected_%d", (int)value);
                } else {
                    if (!reply_push(engine, kind, false, NULL)) return;
                    engine->replies[engine->reply_count - 1u].ival = value;
                }
            } else {
                if (!reply_push(engine, kind, false, NULL)) return;
                engine->replies[engine->reply_count - 1u].ival = value;
            }
        }
    }
    if (pending != NULL) pending->used = false;
}

static void handle_name_owner_changed(cp_spp_dbus *engine,
                                      DBusMessage *message)
{
    const char *name = NULL, *old_owner = NULL, *new_owner = NULL;
    if (!driver_sender(message) ||
        !dbus_message_is_signal(message, "org.freedesktop.DBus",
                                "NameOwnerChanged") ||
        !signature_is(message, "sss") ||
        !dbus_message_get_args(message, NULL,
                               DBUS_TYPE_STRING, &name,
                               DBUS_TYPE_STRING, &old_owner,
                               DBUS_TYPE_STRING, &new_owner,
                               DBUS_TYPE_INVALID) ||
        name == NULL || new_owner == NULL) {
        fatal(engine, "owner_signal_malformed");
        return;
    }
    (void)old_owner;
    bool watched = strcmp(name, engine->adapter_service) == 0;
    if (!watched && engine->connected_service[0])
        watched = strcmp(name, engine->connected_service) == 0;
    for (uint8_t i = 0; !watched && i < engine->list_serial_count; i++) {
        if (strcmp(name, engine->list_serial[i]) == 0) watched = true;
    }
    if (!watched) return;
    if (engine->unique_owner[0] == '\0' ||
        strcmp(new_owner, engine->unique_owner) != 0) {
        engine->owner_lost = true;
    }
}

static void handle_status_signal(cp_spp_dbus *engine, DBusMessage *message)
{
    if (!engine->adapter_validated || engine->unique_owner[0] == '\0') {
        fatal(engine, "status_before_validation");
        return;
    }
    if (!service_sender(engine, message)) {
        fatal(engine, "status_unexpected_owner");
        return;
    }
    const char *path = dbus_message_get_path(message);
    if (!bounded_path(path, CP_SPP_DBUS_MAX_PATH)) {
        fatal(engine, "status_bad_object");
        return;
    }
    char status[CP_SPP_DBUS_MAX_VALUE] = "";
    if (signature_is(message, "ss")) {
        const char *arg_path = NULL, *arg_status = NULL;
        if (!dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_STRING, &arg_path,
                                   DBUS_TYPE_STRING, &arg_status,
                                   DBUS_TYPE_INVALID) ||
            arg_path == NULL || arg_status == NULL ||
            !bounded_path(arg_path, CP_SPP_DBUS_MAX_PATH) ||
            strcmp(arg_path, path) != 0 ||
            !bounded_text(arg_status, CP_SPP_DBUS_MAX_VALUE - 1u)) {
            fatal(engine, "status_bad_payload");
            return;
        }
        (void)copy_bounded(status, sizeof(status), arg_status);
    } else if (signature_is(message, "s")) {
        const char *arg_status = NULL;
        if (!dbus_message_get_args(message, NULL,
                                   DBUS_TYPE_STRING, &arg_status,
                                   DBUS_TYPE_INVALID) ||
            arg_status == NULL ||
            !bounded_text(arg_status, CP_SPP_DBUS_MAX_VALUE - 1u)) {
            fatal(engine, "status_bad_payload");
            return;
        }
        (void)copy_bounded(status, sizeof(status), arg_status);
    } else {
        fatal(engine, "status_bad_signature");
        return;
    }
    if (engine->status_pending) {
        if (!strcmp(engine->status_path, path) &&
            !strcmp(engine->status_value, status)) return;
        fatal(engine, "status_flood");
        return;
    }
    const char *signature = dbus_message_get_signature(message);
    if (engine->connected &&
        strcmp(signature, engine->serial_status_signature)) {
        fatal(engine, "status_signature_changed");
        return;
    }
    (void)copy_bounded(engine->status_signature,
                       sizeof(engine->status_signature), signature);
    if (!copy_bounded(engine->status_path, sizeof(engine->status_path), path) ||
        !copy_bounded(engine->status_value, sizeof(engine->status_value),
                      status)) {
        fatal(engine, "status_too_long");
        return;
    }
    engine->status_pending = true;
}

static DBusHandlerResult dbus_filter(DBusConnection *connection,
                                     DBusMessage *message, void *data)
{
    cp_spp_dbus *engine = data;
    (void)connection;
    if (engine == NULL) return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
    if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_METHOD_RETURN ||
        dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_ERROR) {
        handle_reply(engine, message);
    } else if (dbus_message_get_type(message) == DBUS_MESSAGE_TYPE_SIGNAL) {
        if (dbus_message_is_signal(message, "org.freedesktop.DBus",
                                   "NameOwnerChanged")) {
            handle_name_owner_changed(engine, message);
        } else if (dbus_message_is_signal(
                       message, engine->serial_interface,
                       CP_SPP_DBUS_SERIAL_SIGNAL_STATUS)) {
            handle_status_signal(engine, message);
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

/* ---------------------------------------------------------------- */
/* reply/state processing                                            */
/* ---------------------------------------------------------------- */

static bool send_add_matches(cp_spp_dbus *engine, uint64_t now_ms)
{
    char rule[512];
    snprintf(rule, sizeof(rule),
             "type='signal',interface='%s',member='%s'",
             engine->serial_interface, CP_SPP_DBUS_SERIAL_SIGNAL_STATUS);
    if (!send_string_call(engine, "org.freedesktop.DBus",
                          "/org/freedesktop/DBus", "org.freedesktop.DBus",
                          "AddMatch", rule, NULL, CP_SPP_DBUS_PK_MATCH_STATUS,
                          now_ms, NULL))
        return false;
    snprintf(rule, sizeof(rule),
             "type='signal',sender='org.freedesktop.DBus',"
             "interface='org.freedesktop.DBus',member='NameOwnerChanged'");
    return send_string_call(engine, "org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "AddMatch", rule, NULL,
                            CP_SPP_DBUS_PK_MATCH_OWNER, now_ms, NULL);
}

static bool send_list_names(cp_spp_dbus *engine, uint64_t now_ms)
{
    return send_noarg_call(engine, "org.freedesktop.DBus",
                           "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "ListNames", CP_SPP_DBUS_PK_LIST_NAMES, now_ms,
                           NULL);
}

static bool send_owner(cp_spp_dbus *engine, const char *name,
                       cp_spp_dbus_pk kind, uint64_t now_ms)
{
    return send_string_call(engine, "org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "GetNameOwner", name, NULL, kind, now_ms, name);
}

static bool send_uid(cp_spp_dbus *engine, uint64_t now_ms)
{
    return send_string_call(engine, "org.freedesktop.DBus",
                            "/org/freedesktop/DBus", "org.freedesktop.DBus",
                            "GetConnectionUnixUser", engine->unique_owner,
                            NULL, CP_SPP_DBUS_PK_UID, now_ms, NULL);
}

static bool send_introspect(cp_spp_dbus *engine, const char *path,
                            cp_spp_dbus_pk kind, uint64_t now_ms)
{
    return send_noarg_call(engine, engine->unique_owner, path,
                           "org.freedesktop.DBus.Introspectable",
                           "Introspect", kind, now_ms, path);
}

static void process_hello(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (!send_add_matches(engine, now_ms))
        fatal(engine, "match_send_failed");
}

static void process_match(cp_spp_dbus *engine, uint64_t now_ms)
{
    engine->match_done++;
    if (engine->match_done == 2u && !send_list_names(engine, now_ms))
        fatal(engine, "list_names_send_failed");
}

static void process_list_names(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine->list_invalid) {
        fatal(engine, "list_names_malformed");
        return;
    }
    if (!engine->list_adapter_found) {
        fatal(engine, "adapter_not_found");
        return;
    }
    engine->owner_wait = (uint8_t)(1u + engine->list_serial_count);
    memset(engine->serial_owner, 0, sizeof(engine->serial_owner));
    if (!send_owner(engine, engine->adapter_service,
                    CP_SPP_DBUS_PK_OWNER_ADAPTER, now_ms)) {
        fatal(engine, "owner_query_failed");
        return;
    }
    for (uint8_t i = 0; i < engine->list_serial_count; i++) {
        if (!send_owner(engine, engine->list_serial[i],
                        CP_SPP_DBUS_PK_OWNER_SERIAL, now_ms)) {
            fatal(engine, "owner_query_failed");
            return;
        }
    }
}

static void process_owner(cp_spp_dbus *engine, cp_spp_dbus_reply *reply,
                          uint64_t now_ms)
{
    bool is_adapter = reply->kind == CP_SPP_DBUS_PK_OWNER_ADAPTER;
    if (reply->sval[0] != ':') {
        fatal(engine, "owner_unexpected");
        return;
    }
    if (is_adapter) {
        (void)copy_bounded(engine->unique_owner,
                           sizeof(engine->unique_owner), reply->sval);
    } else {
        for (uint8_t i = 0; i < engine->list_serial_count; i++) {
            if (strcmp(reply->aux, engine->list_serial[i]) == 0) {
                (void)copy_bounded(engine->serial_owner[i],
                                   sizeof(engine->serial_owner[0]),
                                   reply->sval);
                break;
            }
        }
    }
    if (engine->owner_wait > 0) engine->owner_wait--;
    if (engine->owner_wait != 0) return;
    if (engine->unique_owner[0] != ':') {
        fatal(engine, "adapter_owner_missing");
        return;
    }
    for (uint8_t i = 0; i < engine->list_serial_count; i++) {
        if (strcmp(engine->serial_owner[i], engine->unique_owner) != 0) {
            fatal(engine, "multiple_service_owners");
            return;
        }
    }
    if (!send_uid(engine, now_ms))
        fatal(engine, "uid_query_failed");
}

static void process_uid(cp_spp_dbus *engine, cp_spp_dbus_reply *reply,
                        uint64_t now_ms)
{
    uint32_t expected = engine->expected_owner_uid;
    if (reply->uval != expected) {
        fatal(engine, "unexpected_owner_uid");
        return;
    }
    if (!send_introspect(engine, engine->adapter_path,
                         CP_SPP_DBUS_PK_XML_ADAPTER, now_ms))
        fatal(engine, "adapter_introspect_failed");
}

static void process_xml_adapter(cp_spp_dbus *engine, cp_spp_dbus_reply *reply,
                                uint64_t now_ms)
{
    const char *xml = NULL;
    int length = 0;
    if (reply->xml == NULL ||
        !dbus_message_get_args(reply->xml, NULL,
                               DBUS_TYPE_STRING, &xml,
                               DBUS_TYPE_INVALID) ||
        xml == NULL) {
        fatal(engine, "adapter_xml_missing");
        return;
    }
    length = (int)strlen(xml);
    if (length <= 0 || (size_t)length > CP_SPP_DBUS_MAX_XML_BYTES) {
        fatal(engine, "adapter_xml_too_large");
        return;
    }
    uint8_t args = 0;
    uint8_t stop_args = 0;
    if (!introspect_listen(xml, (size_t)length, engine->adapter_interface, &args) ||
        !introspect_stop(xml, (size_t)length, engine->adapter_interface,
                         &stop_args)) {
        fatal(engine, "adapter_method_signature");
        return;
    }
    engine->listen_args = args;
    engine->stop_args = stop_args;
    engine->adapter_validated = true;
    engine->validation_deadline_ms = 0;
    select_attach_candidate(engine);
    if (engine->state == CP_SPP_DBUS_STATE_FATAL) return;
    set_state(engine, CP_SPP_DBUS_STATE_LISTENING);
    if (engine->cb.on_validated)
        engine->cb.on_validated(engine->user);
    (void)now_ms;
}

static void process_listen(cp_spp_dbus *engine, cp_spp_dbus_reply *reply,
                           uint64_t now_ms)
{
    engine->listen_pending = false;
    if (reply->ival == 0) {
        engine->listener_owned = true;
        /* A SerialPort object that already existed at validation is attached
         * only now: our own listener owns the transport, and the owner lookup
         * is repeated before the object can become CONNECTED. */
        if (engine->attach_pending) {
            if (!copy_bounded(engine->pending_path,
                              sizeof(engine->pending_path),
                              engine->attach_path) ||
                !copy_bounded(engine->connected_service,
                              sizeof(engine->connected_service),
                              engine->attach_service)) {
                fatal(engine, "attach_path_too_long");
                return;
            }
            engine->serial_pending = true;
            if (!send_owner(engine, engine->attach_service,
                            CP_SPP_DBUS_PK_OWNER_CONNECTED, now_ms))
                fatal(engine, "connected_owner_query_failed");
        }
        return;
    }
    /* Never assume ownership of a foreign/duplicate listener. */
    fatal(engine, "listen_rejected_%d", (int)reply->ival);
}

static void process_stop(cp_spp_dbus *engine, cp_spp_dbus_reply *reply)
{
    engine->stop_pending = false;
    if (reply->ival != 0) {
        fatal(engine, "stop_rejected_%d", (int)reply->ival);
        return;
    }
    engine->listener_owned = false;
    engine->connected = false;
    engine->rx_peer[0] = '\0';
    if (engine->connected_path[0] != '\0') {
        char removed[CP_SPP_DBUS_MAX_PATH];
        (void)copy_bounded(removed, sizeof(removed), engine->connected_path);
        engine->connected_path[0] = '\0';
        if (engine->cb.on_removed)
            engine->cb.on_removed(engine->user, removed);
    }
    set_state(engine, CP_SPP_DBUS_STATE_LISTENING);
}

static void process_xml_serial(cp_spp_dbus *engine, cp_spp_dbus_reply *reply,
                               uint64_t now_ms)
{
    const char *xml = NULL;
    int length = 0;
    bool was_attach = engine->attach_pending;
    if (!engine->serial_pending ||
        strcmp(reply->aux, engine->pending_path) != 0) {
        /* A late reply for an object that disconnected or was superseded. */
        return;
    }
    if (reply->xml == NULL ||
        !dbus_message_get_args(reply->xml, NULL,
                               DBUS_TYPE_STRING, &xml,
                               DBUS_TYPE_INVALID) ||
        xml == NULL) {
        fatal(engine, "serial_xml_missing");
        return;
    }
    length = (int)strlen(xml);
    if (length <= 0 || (size_t)length > CP_SPP_DBUS_MAX_XML_BYTES) {
        fatal(engine, "serial_xml_too_large");
        return;
    }
    char signal_in[8] = "", signal_out[8] = "";
    if (!introspect_send(xml, (size_t)length, engine->serial_interface) ||
        !introspect_method(xml, (size_t)length, engine->serial_interface,
            CP_SPP_DBUS_SERIAL_SIGNAL_STATUS, signal_in, sizeof(signal_in),
            signal_out, sizeof(signal_out), true) || signal_in[0]) {
        fatal(engine, "senddata_method_signature");
        return;
    }
    /* Attach has no status signal to learn the signature from; the object's
     * own introspection is the source.  A later signal must match it. */
    if (engine->status_signature[0] == '\0') {
        if (!copy_bounded(engine->status_signature,
                          sizeof(engine->status_signature), signal_out)) {
            fatal(engine, "serial_signal_too_long");
            return;
        }
    } else if (strcmp(signal_out, engine->status_signature) != 0) {
        fatal(engine, "senddata_method_signature");
        return;
    }
    if (!copy_bounded(engine->connected_path, sizeof(engine->connected_path),
                      engine->pending_path)) {
        fatal(engine, "serial_path_too_long");
        return;
    }
    engine->serial_pending = false;
    engine->attach_pending = false;
    engine->attached = was_attach;
    engine->connected = true;
    if (engine->connect_generation == UINT32_MAX) {
        fatal(engine, "connect_generation_exhausted");
        return;
    }
    engine->connect_generation++;
    (void)copy_bounded(engine->serial_status_signature,
                       sizeof(engine->serial_status_signature), signal_out);
    if (!serial_identity(engine, engine->connected_path, engine->rx_peer,
                          engine->connected_service)) {
        fatal(engine, "serial_identity_invalid");
        return;
    }
    set_state(engine, CP_SPP_DBUS_STATE_CONNECTED);
    (void)now_ms;
}

static void process_send(cp_spp_dbus *engine, cp_spp_dbus_reply *reply)
{
    if (reply->ival != 0) {
        fatal(engine, "send_rejected_%d", (int)reply->ival);
        return;
    }
    uint32_t length = engine->inflight_len;
    if (engine->tx_count > 0) {
        engine->tx_head = (uint8_t)((engine->tx_head + 1u) %
                                    CP_SPP_DBUS_MAX_TX_QUEUE);
        engine->tx_count--;
    }
    engine->inflight_len = 0;
    engine->send_deadline_ms = 0;
    if (engine->cb.on_stub_ack)
        engine->cb.on_stub_ack(engine->user, length);
}

static bool process_replies(cp_spp_dbus *engine, uint64_t now_ms)
{
    uint8_t count = engine->reply_count;
    engine->reply_count = 0;
    for (uint8_t i = 0; i < count; i++) {
        cp_spp_dbus_reply reply = engine->replies[i];
        engine->replies[i].xml = NULL;
        if (engine->state == CP_SPP_DBUS_STATE_FATAL) {
            if (reply.xml != NULL) dbus_message_unref(reply.xml);
            continue;
        }
        if (reply.error) {
            if (reply.kind == CP_SPP_DBUS_PK_SEND)
                engine->send_inflight = false;
            fatal(engine, "dbus_error_%s", reply.eval);
            if (reply.xml != NULL) dbus_message_unref(reply.xml);
            continue;
        }
        switch (reply.kind) {
        case CP_SPP_DBUS_PK_HELLO:
            process_hello(engine, now_ms);
            break;
        case CP_SPP_DBUS_PK_MATCH_STATUS:
        case CP_SPP_DBUS_PK_MATCH_OWNER:
            process_match(engine, now_ms);
            break;
        case CP_SPP_DBUS_PK_LIST_NAMES:
            process_list_names(engine, now_ms);
            break;
        case CP_SPP_DBUS_PK_OWNER_ADAPTER:
        case CP_SPP_DBUS_PK_OWNER_SERIAL:
            process_owner(engine, &reply, now_ms);
            break;
        case CP_SPP_DBUS_PK_OWNER_CONNECTED:
            if (strcmp(reply.sval, engine->unique_owner) != 0 ||
                strcmp(reply.aux, engine->connected_service) != 0) {
                fatal(engine, "connected_owner_mismatch");
            } else if (engine->serial_pending &&
                       !send_introspect(engine, engine->pending_path,
                                        CP_SPP_DBUS_PK_XML_SERIAL, now_ms)) {
                fatal(engine, "serial_introspect_failed");
            }
            break;
        case CP_SPP_DBUS_PK_UID:
            process_uid(engine, &reply, now_ms);
            break;
        case CP_SPP_DBUS_PK_XML_ADAPTER:
            process_xml_adapter(engine, &reply, now_ms);
            break;
        case CP_SPP_DBUS_PK_LISTEN:
            process_listen(engine, &reply, now_ms);
            break;
        case CP_SPP_DBUS_PK_STOP:
            process_stop(engine, &reply);
            break;
        case CP_SPP_DBUS_PK_XML_SERIAL:
            process_xml_serial(engine, &reply, now_ms);
            break;
        case CP_SPP_DBUS_PK_SEND:
            process_send(engine, &reply);
            break;
        default:
            break;
        }
        if (reply.xml != NULL) dbus_message_unref(reply.xml);
    }
    return engine->state != CP_SPP_DBUS_STATE_FATAL;
}

static void process_status(cp_spp_dbus *engine, uint64_t now_ms)
{
    char path[CP_SPP_DBUS_MAX_PATH];
    char status[CP_SPP_DBUS_MAX_VALUE];
    (void)copy_bounded(path, sizeof(path), engine->status_path);
    (void)copy_bounded(status, sizeof(status), engine->status_value);
    engine->status_pending = false;
    if (strcmp(status, "connected") == 0) {
        if (engine->state == CP_SPP_DBUS_STATE_CONNECTED) {
            if (strcmp(path, engine->connected_path) != 0) {
                fatal(engine, "connected_path_changed");
                return;
            }
            if (engine->cb.on_status)
                engine->cb.on_status(engine->user, path, status);
            return;
        }
        if (engine->state != CP_SPP_DBUS_STATE_LISTENING ||
            !engine->listener_owned) {
            fatal(engine, "connected_before_listen");
            return;
        }
        if (engine->serial_pending) {
            if (strcmp(path, engine->pending_path) != 0) {
                fatal(engine, "second_connected_object");
                return;
            }
            if (engine->cb.on_status)
                engine->cb.on_status(engine->user, path, status);
            return;
        }
        if (!copy_bounded(engine->pending_path, sizeof(engine->pending_path),
                          path)) {
            fatal(engine, "connected_path_too_long");
            return;
        }
        if (engine->cb.on_status)
            engine->cb.on_status(engine->user, path, status);
        engine->serial_pending = true;
        char peer[18];
        if (!serial_identity(engine, path, peer, engine->connected_service)) {
            fatal(engine, "serial_identity_invalid");
            return;
        }
        if (!send_owner(engine, engine->connected_service,
                         CP_SPP_DBUS_PK_OWNER_CONNECTED, now_ms))
            fatal(engine, "connected_owner_query_failed");
        return;
    }
    if (strcmp(status, "disconnected") == 0) {
        if (engine->cb.on_status)
            engine->cb.on_status(engine->user, path, status);
        if (engine->serial_pending &&
            strcmp(path, engine->pending_path) == 0) {
            engine->serial_pending = false;
            engine->pending_path[0] = '\0';
        }
        if (engine->connected &&
            strcmp(path, engine->connected_path) == 0) {
            engine->connected = false;
            engine->rx_peer[0] = '\0';
            engine->connected_path[0] = '\0';
            engine->connected_service[0] = '\0';
            if (engine->send_inflight)
                engine->ignored_send_serial = engine->send_serial;
            engine->tx_count = 0;
            engine->tx_head = 0;
            engine->send_inflight = false;
            engine->inflight_len = 0;
            engine->send_deadline_ms = 0;
            if (engine->cb.on_removed)
                engine->cb.on_removed(engine->user, path);
            /* A disconnect is a normal end of one connection, not a reason to
             * discard the listener.  A later connected confirmation advances
             * connect_generation so the caller can reset per-connection state
             * even when the same phone reconnects. */
            set_state(engine, CP_SPP_DBUS_STATE_LISTENING);
        }
        return;
    }
    /* A non-connect status is informational only.  It never fabricates a
     * connected state and never satisfies a send. */
    if (engine->cb.on_status)
        engine->cb.on_status(engine->user, path, status);
}

static bool pump(cp_spp_dbus *engine)
{
    if (!dbus_connection_read_write(engine->conn, 0)) {
        fatal(engine, "bus_disconnected");
        return false;
    }
    for (unsigned i = 0; i < 64u; i++) {
        DBusDispatchStatus status = dbus_connection_dispatch(engine->conn);
        if (status == DBUS_DISPATCH_COMPLETE) return true;
        if (status == DBUS_DISPATCH_NEED_MEMORY) {
            fatal(engine, "bus_dispatch_memory");
            return false;
        }
    }
    fatal(engine, "bus_dispatch_flood");
    return false;
}

static void check_deadlines(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine->state == CP_SPP_DBUS_STATE_FATAL) return;
    if (engine->validation_deadline_ms != 0 &&
        now_ms >= engine->validation_deadline_ms) {
        fatal(engine, "validation_timeout");
        return;
    }
    for (size_t i = 0; i < CP_SPP_DBUS_MAX_PENDING; i++) {
        if (engine->pending[i].used &&
            now_ms >= engine->pending[i].deadline_ms) {
            fatal(engine, "control_timeout");
            return;
        }
    }
    if (engine->send_inflight &&
        engine->send_deadline_ms != 0 &&
        now_ms >= engine->send_deadline_ms) {
        engine->send_inflight = false;
        fatal(engine, "send_timeout");
    }
}

static bool send_frame(cp_spp_dbus *engine, uint64_t now_ms)
{
    cp_spp_dbus_frame *frame = &engine->tx[engine->tx_head];
    DBusMessage *message = dbus_message_new_method_call(
        engine->unique_owner, engine->connected_path,
        engine->serial_interface, CP_SPP_DBUS_SERIAL_METHOD_SEND_DATA);
    DBusMessageIter iter, array;
    dbus_uint32_t serial = 0;
    if (message == NULL) return false;
    dbus_message_set_auto_start(message, FALSE);
    dbus_message_iter_init_append(message, &iter);
    if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
                                          DBUS_TYPE_BYTE_AS_STRING, &array)) {
        dbus_message_unref(message);
        fatal(engine, "message_build_failed");
        return false;
    }
    for (size_t i = 0; i < frame->len; i++) {
        unsigned char byte = frame->bytes[i];
        if (!dbus_message_iter_append_basic(&array, DBUS_TYPE_BYTE, &byte)) {
            dbus_message_iter_abandon_container(&iter, &array);
            dbus_message_unref(message);
            fatal(engine, "message_build_failed");
            return false;
        }
    }
    if (!dbus_message_iter_close_container(&iter, &array) ||
        !dbus_connection_send(engine->conn, message, &serial) || serial == 0) {
        dbus_message_unref(message);
        fatal(engine, "send_failed");
        return false;
    }
    dbus_message_unref(message);
    engine->send_serial = serial;
    engine->send_inflight = true;
    engine->inflight_len = frame->len;
    engine->send_deadline_ms = deadline_add(now_ms, engine->send_timeout_ms);
    return true;
}

/* ---------------------------------------------------------------- */
/* public API                                                        */
/* ---------------------------------------------------------------- */

int cp_spp_dbus_open(cp_spp_dbus **engine_out, cp_spp_dbus_storage *storage,
                     const cp_spp_dbus_config *config,
                     const cp_spp_dbus_callbacks *callbacks, void *user,
                     char *error, size_t error_size)
{
    if (error != NULL && error_size > 0) error[0] = '\0';
    if (engine_out == NULL || storage == NULL || config == NULL ||
        callbacks == NULL) {
        if (error != NULL && error_size > 0)
            snprintf(error, error_size, "invalid argument");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    *engine_out = NULL;
    cp_spp_dbus *engine = (cp_spp_dbus *)storage;
    memset(engine, 0, sizeof(*engine));
    engine->cb = *callbacks;
    engine->user = user;
    engine->experimental = config->experimental == 1;
    if (!engine->experimental) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "experimental=1 required");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    if (!bounded_bus_address(config->bus_address)) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "explicit private unix bus_address required");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    if (!bounded_text(config->service_name,
                      CP_SPP_DBUS_MAX_SERVICE - 1u) ||
        config->service_name[0] == '\0') {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "service_name required");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    const char *adapter_service = config->adapter_service != NULL ?
        config->adapter_service : CP_SPP_DBUS_ADAPTER_SERVICE;
    const char *adapter_path = config->adapter_path != NULL ?
        config->adapter_path : CP_SPP_DBUS_ADAPTER_PATH;
    const char *adapter_interface = config->adapter_interface != NULL ?
        config->adapter_interface : CP_SPP_DBUS_ADAPTER_INTERFACE;
    const char *serial_prefix = config->serial_service_prefix != NULL ?
        config->serial_service_prefix : CP_SPP_DBUS_SERIAL_SERVICE_PREFIX;
    const char *serial_interface = config->serial_interface != NULL ?
        config->serial_interface : CP_SPP_DBUS_SERIAL_INTERFACE;
    const char *uuid_accessory = config->uuid_accessory != NULL ?
        config->uuid_accessory : CP_SPP_DBUS_UUID_CAFF;
    const char *uuid_phone = config->uuid_phone != NULL ?
        config->uuid_phone : CP_SPP_DBUS_UUID_CAFE;
    if (!bounded_name(adapter_service, CP_SPP_DBUS_MAX_SERVICE - 1u) ||
        !bounded_name(serial_prefix, CP_SPP_DBUS_MAX_SERVICE - 1u) ||
        !bounded_name(adapter_interface, CP_SPP_DBUS_MAX_IFACE - 1u) ||
        !bounded_name(serial_interface, CP_SPP_DBUS_MAX_IFACE - 1u) ||
        !bounded_path(adapter_path, CP_SPP_DBUS_MAX_PATH - 1u) ||
        !bounded_uuid(uuid_accessory) || !bounded_uuid(uuid_phone) ||
        strcmp(uuid_accessory, uuid_phone) == 0) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "invalid service/path/interface/uuid");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    if (!copy_bounded(engine->bus_address, sizeof(engine->bus_address),
                      config->bus_address) ||
        !copy_bounded(engine->adapter_service,
                      sizeof(engine->adapter_service), adapter_service) ||
        !copy_bounded(engine->adapter_path, sizeof(engine->adapter_path),
                      adapter_path) ||
        !copy_bounded(engine->adapter_interface,
                      sizeof(engine->adapter_interface), adapter_interface) ||
        !copy_bounded(engine->serial_prefix, sizeof(engine->serial_prefix),
                      serial_prefix) ||
        !copy_bounded(engine->serial_interface,
                      sizeof(engine->serial_interface), serial_interface) ||
        !copy_bounded(engine->uuid_accessory,
                      sizeof(engine->uuid_accessory), uuid_accessory) ||
        !copy_bounded(engine->uuid_phone, sizeof(engine->uuid_phone),
                      uuid_phone) ||
        !copy_bounded(engine->service_name, sizeof(engine->service_name),
                      config->service_name)) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "configuration text too long");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    uint32_t control = config->control_timeout_ms != 0 ?
        config->control_timeout_ms : 3000u;
    uint32_t send = config->send_timeout_ms != 0 ?
        config->send_timeout_ms : 1000u;
    if (control > CP_SPP_DBUS_MAX_START_DEADLINE ||
        send > CP_SPP_DBUS_MAX_SEND_DEADLINE) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "timeout out of bounds");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    size_t max_tx = config->max_tx_bytes != 0 ? config->max_tx_bytes :
                    CP_SPP_DBUS_MAX_TX_BYTES;
    if (max_tx > CP_SPP_DBUS_MAX_TX_BYTES) {
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "max_tx_bytes exceeds %u", CP_SPP_DBUS_MAX_TX_BYTES);
        return CP_SPP_DBUS_ERR_INVALID;
    }
    engine->control_timeout_ms = control;
    engine->send_timeout_ms = send;
    engine->max_tx_bytes = max_tx;
    engine->expected_owner_uid = config->expected_owner_uid_set ?
        config->expected_owner_uid : 0u;

    DBusError dbus_error;
    dbus_error_init(&dbus_error);
    engine->conn = dbus_connection_open_private(engine->bus_address,
                                                &dbus_error);
    if (engine->conn == NULL) {
        const char *why = dbus_error_is_set(&dbus_error) ?
            dbus_error.message : "cannot open private bus";
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "bus open failed: %s", why ? why : "unknown");
        dbus_error_free(&dbus_error);
        memset(engine, 0, sizeof(*engine));
        return CP_SPP_DBUS_ERR_INVALID;
    }
    dbus_error_free(&dbus_error);
    dbus_connection_set_exit_on_disconnect(engine->conn, FALSE);
    dbus_connection_set_max_message_size(engine->conn, 131072L);
    dbus_connection_set_max_received_size(engine->conn, 524288L);
    dbus_connection_set_max_message_unix_fds(engine->conn, 0L);
    /* A zero aggregate FD budget also stops fd-free messages from being
     * read by libdbus. Keep a small transport budget; the per-message zero
     * limit and exact signatures still reject descriptor-bearing messages. */
    dbus_connection_set_max_received_unix_fds(engine->conn, 8L);
    if (!dbus_connection_add_filter(engine->conn, dbus_filter, engine, NULL)) {
        dbus_connection_close(engine->conn);
        dbus_connection_unref(engine->conn);
        memset(engine, 0, sizeof(*engine));
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "cannot install filter");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    int fd = -1;
    if (!dbus_connection_get_socket(engine->conn, &fd) || fd < 0) {
        dbus_connection_remove_filter(engine->conn, dbus_filter, engine);
        dbus_connection_close(engine->conn);
        dbus_connection_unref(engine->conn);
        memset(engine, 0, sizeof(*engine));
        snprintf(error && error_size ? error : engine->error,
                 error && error_size ? error_size : sizeof(engine->error),
                 "bus socket unavailable");
        return CP_SPP_DBUS_ERR_INVALID;
    }
    engine->fd = fd;
    engine->opened = true;
    engine->state = CP_SPP_DBUS_STATE_OPEN;
    *engine_out = engine;
    return CP_SPP_DBUS_OK;
}

int cp_spp_dbus_start(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine == NULL || !engine->opened)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state != CP_SPP_DBUS_STATE_OPEN)
        return CP_SPP_DBUS_ERR_STATE;
    if (now_ms < engine->last_now_ms)
        return CP_SPP_DBUS_ERR_INVALID;
    engine->last_now_ms = now_ms;
    engine->validation_deadline_ms =
        deadline_add(now_ms, engine->control_timeout_ms);
    set_state(engine, CP_SPP_DBUS_STATE_VALIDATING);
    DBusMessage *message = dbus_message_new_method_call(
        "org.freedesktop.DBus", "/org/freedesktop/DBus",
        "org.freedesktop.DBus", "Hello");
    if (!send_message(engine, message, CP_SPP_DBUS_PK_HELLO, now_ms, NULL)) {
        fatal(engine, "hello_send_failed");
        return CP_SPP_DBUS_ERR_STATE;
    }
    return CP_SPP_DBUS_OK;
}

int cp_spp_dbus_tick(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine == NULL || !engine->opened)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state == CP_SPP_DBUS_STATE_FATAL)
        return CP_SPP_DBUS_ERR_STATE;
    if (now_ms < engine->last_now_ms) {
        fatal(engine, "clock_rollback");
        return CP_SPP_DBUS_ERR_STATE;
    }
    engine->last_now_ms = now_ms;
    check_deadlines(engine, now_ms);
    if (engine->state == CP_SPP_DBUS_STATE_FATAL)
        return CP_SPP_DBUS_ERR_STATE;
    if (!pump(engine)) return CP_SPP_DBUS_ERR_STATE;
    if (engine->owner_lost) {
        fatal(engine, "owner_lost");
        return CP_SPP_DBUS_ERR_STATE;
    }
    if (!process_replies(engine, now_ms))
        return CP_SPP_DBUS_ERR_STATE;
    if (engine->status_pending && !engine->listen_pending)
        process_status(engine, now_ms);
    if (engine->state == CP_SPP_DBUS_STATE_FATAL)
        return CP_SPP_DBUS_ERR_STATE;
    check_deadlines(engine, now_ms);
    if (engine->state == CP_SPP_DBUS_STATE_FATAL)
        return CP_SPP_DBUS_ERR_STATE;
    if (engine->state == CP_SPP_DBUS_STATE_CONNECTED &&
        !engine->send_inflight && engine->tx_count > 0)
        (void)send_frame(engine, now_ms);
    return engine->state == CP_SPP_DBUS_STATE_FATAL ?
        CP_SPP_DBUS_ERR_STATE : CP_SPP_DBUS_OK;
}

int cp_spp_dbus_listen(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine == NULL || !engine->opened)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state != CP_SPP_DBUS_STATE_LISTENING ||
        !engine->adapter_validated || engine->listener_owned ||
        engine->listen_pending || engine->stop_pending)
        return CP_SPP_DBUS_ERR_STATE;
    if (now_ms < engine->last_now_ms)
        return CP_SPP_DBUS_ERR_INVALID;
    engine->last_now_ms = now_ms;
    bool sent;
    if (engine->listen_args == 2u) {
        sent = send_string_call(engine, engine->unique_owner,
                                engine->adapter_path,
                                engine->adapter_interface, "ListenSPP",
                                engine->uuid_accessory, engine->service_name,
                                CP_SPP_DBUS_PK_LISTEN, now_ms, NULL);
    } else {
        sent = send_string_call(engine, engine->unique_owner,
                                engine->adapter_path,
                                engine->adapter_interface, "ListenSPP",
                                engine->service_name, NULL,
                                CP_SPP_DBUS_PK_LISTEN, now_ms, NULL);
    }
    if (!sent) return CP_SPP_DBUS_ERR_STATE;
    engine->listen_pending = true;
    return CP_SPP_DBUS_OK;
}

static bool send_stop_call(cp_spp_dbus *engine, uint64_t now_ms,
                           cp_spp_dbus_pk kind)
{
    if (engine->stop_args == 2u) {
        return send_string_call(engine, engine->unique_owner,
                                engine->adapter_path,
                                engine->adapter_interface, "StopListenSPP",
                                engine->uuid_accessory, engine->service_name,
                                kind, now_ms, NULL);
    }
    return send_string_call(engine, engine->unique_owner, engine->adapter_path,
                            engine->adapter_interface, "StopListenSPP",
                            engine->service_name, NULL, kind, now_ms, NULL);
}

int cp_spp_dbus_stop(cp_spp_dbus *engine, uint64_t now_ms)
{
    if (engine == NULL || !engine->opened)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state == CP_SPP_DBUS_STATE_FATAL)
        return CP_SPP_DBUS_ERR_STATE;
    /* Idempotent: a second/foreign stop never emits a method call.  Only a
     * listener this process created is ever stopped. */
    if (!engine->listener_owned || engine->stop_pending)
        return CP_SPP_DBUS_OK;
    if (now_ms < engine->last_now_ms)
        return CP_SPP_DBUS_ERR_INVALID;
    engine->last_now_ms = now_ms;
    if (engine->send_inflight || engine->tx_count || engine->serial_pending)
        return CP_SPP_DBUS_ERR_STATE;
    if (!send_stop_call(engine, now_ms, CP_SPP_DBUS_PK_STOP))
        return CP_SPP_DBUS_ERR_STATE;
    engine->stop_pending = true;
    engine->connected = false;
    engine->serial_pending = false;
    engine->pending_path[0] = '\0';
    engine->rx_peer[0] = '\0';
    if (engine->connected_path[0] != '\0') {
        char removed[CP_SPP_DBUS_MAX_PATH];
        (void)copy_bounded(removed, sizeof(removed), engine->connected_path);
        engine->connected_path[0] = '\0';
        if (engine->cb.on_removed)
            engine->cb.on_removed(engine->user, removed);
    }
    return CP_SPP_DBUS_OK;
}

int cp_spp_dbus_send(cp_spp_dbus *engine, const uint8_t *data, size_t length,
                     uint64_t now_ms)
{
    if (engine == NULL || !engine->opened || data == NULL || length == 0)
        return CP_SPP_DBUS_ERR_INVALID;
    if (length > engine->max_tx_bytes ||
        length > CP_SPP_DBUS_MAX_TX_BYTES)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state != CP_SPP_DBUS_STATE_CONNECTED || !engine->connected ||
        engine->serial_pending)
        return CP_SPP_DBUS_ERR_NOT_CONNECTED;
    if (now_ms < engine->last_now_ms)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->tx_count >= CP_SPP_DBUS_MAX_TX_QUEUE)
        return CP_SPP_DBUS_ERR_BACKPRESSURE;
    engine->last_now_ms = now_ms;
    uint8_t tail = (uint8_t)((engine->tx_head + engine->tx_count) %
                             CP_SPP_DBUS_MAX_TX_QUEUE);
    cp_spp_dbus_frame *frame = &engine->tx[tail];
    frame->len = (uint16_t)length;
    memcpy(frame->bytes, data, length);
    engine->tx_count++;
    return CP_SPP_DBUS_OK;
}

int cp_spp_dbus_deliver_rx(cp_spp_dbus *engine, const char *peer,
                           uint8_t uuid_kind, const uint8_t *data,
                           size_t length)
{
    if (engine == NULL || !engine->opened)
        return CP_SPP_DBUS_ERR_INVALID;
    if (engine->state != CP_SPP_DBUS_STATE_CONNECTED || !engine->connected ||
        engine->serial_pending)
        return CP_SPP_DBUS_ERR_NOT_CONNECTED;
    if (uuid_kind != CP_SPP_DBUS_RX_UUID_KIND_CAFF || peer == NULL ||
        data == NULL || length == 0 ||
        length > CP_SPP_DBUS_MAX_RX_BYTES || !bounded_mac(peer))
        return CP_SPP_DBUS_ERR_REJECTED;
    if (engine->rx_peer[0] == '\0' || !mac_equal_ci(engine->rx_peer, peer)) {
        return CP_SPP_DBUS_ERR_REJECTED;
    }
    if (engine->cb.on_rx)
        engine->cb.on_rx(engine->user, engine->rx_peer, uuid_kind, data,
                         length);
    return CP_SPP_DBUS_OK;
}

int cp_spp_dbus_pollfd(const cp_spp_dbus *engine, struct pollfd *pfd)
{
    if (engine == NULL || !engine->opened || pfd == NULL) return -1;
    pfd->fd = engine->fd;
    pfd->events = POLLIN;
    if (dbus_connection_get_outgoing_size(engine->conn) > 0)
        pfd->events |= POLLOUT;
    pfd->revents = 0;
    return engine->fd >= 0 ? 1 : 0;
}

int cp_spp_dbus_timeout_ms(const cp_spp_dbus *engine, uint64_t now_ms)
{
    uint64_t deadline;
    if (cp_spp_dbus_next_deadline(engine, &deadline) != 0 ||
        deadline == UINT64_MAX)
        return -1;
    if (now_ms >= deadline) return 0;
    uint64_t remaining = deadline - now_ms;
    return remaining > (uint64_t)INT_MAX ? INT_MAX : (int)remaining;
}

int cp_spp_dbus_next_deadline(const cp_spp_dbus *engine,
                              uint64_t *deadline_ms)
{
    if (engine == NULL || deadline_ms == NULL) return -1;
    uint64_t deadline = UINT64_MAX;
    if (engine->validation_deadline_ms != 0 &&
        engine->validation_deadline_ms < deadline)
        deadline = engine->validation_deadline_ms;
    for (size_t i = 0; i < CP_SPP_DBUS_MAX_PENDING; i++) {
        if (engine->pending[i].used &&
            engine->pending[i].deadline_ms < deadline)
            deadline = engine->pending[i].deadline_ms;
    }
    if (engine->send_inflight && engine->send_deadline_ms != 0 &&
        engine->send_deadline_ms < deadline)
        deadline = engine->send_deadline_ms;
    *deadline_ms = deadline;
    return deadline == UINT64_MAX ? 0 : 1;
}

cp_spp_dbus_state cp_spp_dbus_state_get(const cp_spp_dbus *engine)
{
    return engine != NULL ? engine->state : CP_SPP_DBUS_STATE_FATAL;
}

int cp_spp_dbus_adapter_validated(const cp_spp_dbus *engine)
{
    return engine != NULL && engine->adapter_validated ? 1 : 0;
}

int cp_spp_dbus_listener_owned(const cp_spp_dbus *engine)
{
    return engine != NULL && engine->listener_owned ? 1 : 0;
}

uint32_t cp_spp_dbus_connect_generation(const cp_spp_dbus *engine)
{
    return engine != NULL ? engine->connect_generation : 0u;
}

int cp_spp_dbus_attached(const cp_spp_dbus *engine)
{
    return engine != NULL && engine->attached ? 1 : 0;
}

size_t cp_spp_dbus_tx_queued(const cp_spp_dbus *engine)
{
    if (engine == NULL) return 0;
    /* The in-flight frame remains at the head until its stub ACK. */
    return (size_t)engine->tx_count;
}

const char *cp_spp_dbus_error(const cp_spp_dbus *engine)
{
    return engine != NULL ? engine->error : "";
}

int cp_spp_dbus_peer(const cp_spp_dbus *engine, char *peer, size_t cap)
{
    if (engine == NULL || engine->state != CP_SPP_DBUS_STATE_CONNECTED ||
        !engine->connected || peer == NULL || cap < sizeof(engine->rx_peer) ||
        !bounded_mac(engine->rx_peer)) return CP_SPP_DBUS_ERR_NOT_CONNECTED;
    memcpy(peer, engine->rx_peer, sizeof(engine->rx_peer));
    return CP_SPP_DBUS_OK;
}

void cp_spp_dbus_close(cp_spp_dbus *engine)
{
    if (engine == NULL) return;
    if (engine->conn != NULL) {
        /* Never flush a stale SendData as a side effect of best-effort stop.
         * On fatal/cancelled or outstanding TX, closing the private connection
         * discards its unsent queue. Remote listener shutdown is unconfirmed. */
        if (engine->listener_owned && !engine->owner_lost &&
            !engine->stop_pending &&
            engine->state != CP_SPP_DBUS_STATE_FATAL &&
            !engine->send_inflight && engine->tx_count == 0) {
            DBusMessage *message = dbus_message_new_method_call(
                engine->unique_owner, engine->adapter_path,
                engine->adapter_interface, "StopListenSPP");
            if (message != NULL) {
                dbus_message_set_auto_start(message, FALSE);
                dbus_message_set_no_reply(message, TRUE);
                const char *name = engine->service_name;
                const char *uuid = engine->uuid_accessory;
                bool appended;
                if (engine->stop_args == 2u)
                    appended = dbus_message_append_args(message,
                        DBUS_TYPE_STRING, &uuid, DBUS_TYPE_STRING, &name,
                        DBUS_TYPE_INVALID);
                else
                    appended = dbus_message_append_args(message,
                        DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
                if (appended)
                    dbus_connection_send(engine->conn, message, NULL);
                dbus_message_unref(message);
                dbus_connection_flush(engine->conn);
                (void)dbus_connection_read_write(engine->conn, 0);
            }
        }
        dbus_connection_remove_filter(engine->conn, dbus_filter, engine);
        dbus_connection_close(engine->conn);
        dbus_connection_unref(engine->conn);
    }
    memset(engine, 0, sizeof(*engine));
}
