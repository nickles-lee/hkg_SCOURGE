/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded iAP2 wireless bring-up state machine.  See cp_bootstrap.h for the
 * security invariants.  No allocation, I/O, clocks or logging.
 */

#include "cp_bootstrap.h"

#include <string.h>

static cp_boot_result make_result(const cp_bootstrap *session,
                                  cp_boot_event event, cp_boot_error error,
                                  size_t out_len)
{
    cp_boot_result result;

    memset(&result, 0, sizeof(result));
    result.event = event;
    result.state = session != NULL ? session->state
                                  : CP_BOOT_STATE_DISCONNECTED;
    result.error = error;
    result.projection_active = false;
    result.out_len = out_len;
    result.generation = session != NULL ? session->generation : 0u;
    return result;
}

static cp_boot_result error_result(cp_bootstrap *session, cp_boot_error error)
{
    return make_result(session, CP_BOOT_EV_ERROR, error, 0u);
}

static bool accept_time(cp_bootstrap *session, uint64_t now_ms)
{
    if (now_ms < session->last_now_ms) {
        return false;
    }
    session->last_now_ms = now_ms;
    return true;
}

static uint64_t deadline_after(uint64_t now_ms, uint32_t wait_ms)
{
    if (wait_ms > UINT64_MAX - now_ms) {
        return UINT64_MAX;
    }
    return now_ms + wait_ms;
}

static size_t bounded_len(const char *text, size_t limit)
{
    size_t n = 0;

    if (text == NULL) {
        return limit + 1u;
    }
    while (n <= limit && text[n] != '\0') {
        n++;
    }
    return n;
}

static cp_boot_error copy_text(char *dst, size_t cap, const char *src,
                               size_t min_len, size_t max_len)
{
    size_t n = bounded_len(src, max_len);

    if (dst == NULL || cap == 0u || src == NULL) {
        return CP_BOOT_ERR_CONFIG;
    }
    if (n < min_len || n > max_len || n + 1u > cap) {
        return CP_BOOT_ERR_CONFIG;
    }
    if (!cp_wire_utf8_valid((const uint8_t *)src, n)) {
        return CP_BOOT_ERR_CONFIG;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return CP_BOOT_OK;
}

static cp_boot_error copy_optional_text(char *dst, size_t cap,
                                        const char *src, size_t max_len)
{
    if (src == NULL || src[0] == '\0') {
        dst[0] = '\0';
        return CP_BOOT_OK;
    }
    return copy_text(dst, cap, src, 1u, max_len);
}

static cp_boot_error validate_security(cp_wire_wifi_security security)
{
    switch (security) {
    case CP_WIRE_WIFI_WPA2:
    case CP_WIRE_WIFI_WPA2_WPA3:
    case CP_WIRE_WIFI_WPA3:
        return CP_BOOT_OK;
    default:
        return CP_BOOT_ERR_CONFIG;
    }
}

cp_boot_error cp_bootstrap_init(cp_bootstrap *session,
                                const cp_boot_config *config)
{
    cp_boot_error err;
    size_t i;

    if (session == NULL || config == NULL) {
        return CP_BOOT_ERR_ARGUMENT;
    }
    memset(session, 0, sizeof(*session));
    session->state = CP_BOOT_STATE_DISCONNECTED;
    session->auth_state = CP_BOOT_AUTH_NONE;
    session->ap_state = CP_BOOT_AP_UNKNOWN;

    err = copy_text(session->name, sizeof(session->name), config->name, 1u,
                    CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->model_identifier,
                    sizeof(session->model_identifier),
                    config->model_identifier, 1u, CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->manufacturer, sizeof(session->manufacturer),
                    config->manufacturer, 1u, CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->serial_number, sizeof(session->serial_number),
                    config->serial_number, 1u, CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->firmware_version,
                    sizeof(session->firmware_version),
                    config->firmware_version, 1u, CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->hardware_version,
                    sizeof(session->hardware_version),
                    config->hardware_version, 1u, CP_WIRE_MAX_IDENTITY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->eap_name, sizeof(session->eap_name),
                    config->eap_name, 1u, CP_WIRE_MAX_EAP_NAME);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->current_language,
                    sizeof(session->current_language),
                    config->current_language, 1u, CP_WIRE_MAX_LANGUAGE);
    if (err != CP_BOOT_OK) {
        return err;
    }
    if (config->supported_languages == NULL ||
        config->supported_language_count == 0u ||
        config->supported_language_count > CP_BOOT_MAX_LANGUAGES) {
        return CP_BOOT_ERR_CONFIG;
    }
    session->supported_language_count = config->supported_language_count;
    for (i = 0; i < session->supported_language_count; i++) {
        err = copy_text(session->supported_languages[i],
                        sizeof(session->supported_languages[i]),
                        config->supported_languages[i], 1u,
                        CP_WIRE_MAX_LANGUAGE);
        if (err != CP_BOOT_OK) {
            return err;
        }
    }
    err = copy_optional_text(session->app_match_team_id,
                             sizeof(session->app_match_team_id),
                             config->app_match_team_id, CP_WIRE_MAX_TEAM_ID);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->bluetooth_transport_name,
                    sizeof(session->bluetooth_transport_name),
                    config->bluetooth_transport_name, 1u,
                    CP_WIRE_MAX_TRANSPORT_NAME);
    if (err != CP_BOOT_OK) {
        return err;
    }
    {
        cp_wire_status st = cp_wire_parse_bt_mac(config->bluetooth_mac,
                                                 session->bluetooth_mac);
        if (st != CP_WIRE_OK) {
            return CP_BOOT_ERR_CONFIG;
        }
    }
    session->maximum_current_drawn_ma = config->maximum_current_drawn_ma;

    err = copy_text(session->ap_ssid, sizeof(session->ap_ssid),
                    config->ap_ssid, 1u, CP_WIRE_MAX_SSID);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_text(session->ap_passphrase, sizeof(session->ap_passphrase),
                    config->ap_passphrase, 8u, CP_WIRE_MAX_PASSPHRASE);
    if (err != CP_BOOT_OK) {
        return err;
    }
    if (config->ap_channel == 0u || config->ap_channel > 233u) {
        return CP_BOOT_ERR_CONFIG;
    }
    session->ap_channel = config->ap_channel;
    err = validate_security(config->ap_security);
    if (err != CP_BOOT_OK) {
        return err;
    }
    session->ap_security = config->ap_security;
    if (!cp_wire_link_local_usable(config->link_local)) {
        return CP_BOOT_ERR_CONFIG;
    }
    err = copy_text(session->link_local, sizeof(session->link_local),
                    config->link_local, 1u, CP_WIRE_MAX_LINK_LOCAL);
    if (err != CP_BOOT_OK) {
        return err;
    }
    if (config->airplay_port == 0u || config->airplay_port > 65535u) {
        return CP_BOOT_ERR_CONFIG;
    }
    session->airplay_port = config->airplay_port;
    err = copy_text(session->device_identifier,
                    sizeof(session->device_identifier),
                    config->device_identifier, 1u, CP_WIRE_MAX_DEVICE_ID);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_optional_text(session->public_key, sizeof(session->public_key),
                             config->public_key, CP_WIRE_MAX_PUBLIC_KEY);
    if (err != CP_BOOT_OK) {
        return err;
    }
    err = copy_optional_text(session->source_version,
                             sizeof(session->source_version),
                             config->source_version,
                             CP_WIRE_MAX_SOURCE_VERSION);
    if (err != CP_BOOT_OK) {
        return err;
    }
    if (config->ap_wait_ms == 0u) {
        session->ap_wait_ms = CP_BOOT_DEFAULT_AP_WAIT_MS;
    } else if (config->ap_wait_ms > CP_BOOT_MAX_AP_WAIT_MS) {
        return CP_BOOT_ERR_CONFIG;
    } else {
        session->ap_wait_ms = config->ap_wait_ms;
    }

    session->generation = 1u;
    return CP_BOOT_OK;
}

cp_boot_error cp_bootstrap_set_auth_provider(
    cp_bootstrap *session, const cp_boot_auth_provider *provider)
{
    if (session == NULL) {
        return CP_BOOT_ERR_ARGUMENT;
    }
    if (provider == NULL) {
        memset(&session->auth_provider, 0, sizeof(session->auth_provider));
    } else {
        session->auth_provider = *provider;
    }
    return CP_BOOT_OK;
}

cp_boot_error cp_bootstrap_set_policy(cp_bootstrap *session,
                                      cp_boot_consent_fn consent,
                                      cp_boot_usb_active_fn usb_active,
                                      void *context)
{
    if (session == NULL) {
        return CP_BOOT_ERR_ARGUMENT;
    }
    session->consent = consent;
    session->usb_active = usb_active;
    session->policy_context = context;
    return CP_BOOT_OK;
}

cp_boot_error cp_bootstrap_set_async_auth(cp_bootstrap *session, bool enabled)
{
    if (session == NULL) return CP_BOOT_ERR_ARGUMENT;
    if (session->identification_sent || session->identified ||
        session->auth_state != CP_BOOT_AUTH_NONE)
        return CP_BOOT_ERR_ORDER;
    session->async_auth = enabled;
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    return CP_BOOT_OK;
}

bool cp_bootstrap_auth_request(const cp_bootstrap *session,
                               cp_boot_auth_request *request)
{
    if (session == NULL || request == NULL || !session->async_auth ||
        session->auth_request.op == CP_BOOT_AUTH_OP_NONE ||
        session->auth_request.generation != session->generation ||
        !session->identified || session->state == CP_BOOT_STATE_FAILED)
        return false;
    *request = session->auth_request;
    return true;
}

static void clear_pending(cp_bootstrap *session)
{
    session->pending_start = false;
    session->pending_deadline_ms = 0u;
    session->pending_generation = 0u;
}

static void clear_pending_auth(cp_bootstrap *session)
{
    session->pending_auth_cert = false;
    session->have_pending_auth_challenge = false;
    session->pending_auth_challenge_len = 0u;
    memset(session->pending_auth_challenge, 0,
           sizeof(session->pending_auth_challenge));
}

static void clear_offer(cp_bootstrap *session)
{
    session->offer_active = false;
    session->offer_deadline_ms = 0u;
    session->offer_held = false;
    session->offer_hold_generation = 0u;
}

static void reflect_invalidated_state(cp_bootstrap *session)
{
    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        session->state = CP_BOOT_STATE_AUTHENTICATED;
    } else if (session->identified) {
        session->state = CP_BOOT_STATE_IDENTIFIED;
    } else {
        session->state = CP_BOOT_STATE_DISCONNECTED;
    }
}

static void invalidate_offer(cp_bootstrap *session)
{
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    clear_pending_auth(session);
    clear_pending(session);
    clear_offer(session);
    session->generation++;
    reflect_invalidated_state(session);
}

static void fail_identification(cp_bootstrap *session, cp_boot_event event,
                                cp_boot_error error, cp_boot_result *result)
{
    session->state = CP_BOOT_STATE_FAILED;
    /* Identification failure is terminal for this association.  Clearing the
     * sent/identified/auth state prevents a later 1D02 or team-only 1D03 from
     * reviving the session or starting authentication. */
    session->identification_sent = false;
    session->identified = false;
    session->auth_state = CP_BOOT_AUTH_NONE;
    session->auth_exchanges = 0u;
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    clear_pending_auth(session);
    clear_pending(session);
    clear_offer(session);
    session->generation++;
    *result = make_result(session, event, error, 0u);
}

static void fail_authentication(cp_bootstrap *session, cp_boot_event event,
                                cp_boot_error error, cp_boot_result *result)
{
    session->auth_state = CP_BOOT_AUTH_FAILED;
    session->state = CP_BOOT_STATE_FAILED;
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    clear_pending_auth(session);
    clear_pending(session);
    clear_offer(session);
    session->generation++;
    *result = make_result(session, event, error, 0u);
}

static cp_boot_result begin_async_auth(cp_bootstrap *session,
                                      cp_boot_auth_op op,
                                      const uint8_t *challenge, size_t len)
{
    cp_boot_result result;
    if (session->auth_request.op != CP_BOOT_AUTH_OP_NONE &&
        session->auth_request.generation == session->generation) {
        if (session->auth_request.op == op &&
            session->auth_request.challenge_len == len &&
            (len == 0u || !memcmp(session->auth_request.challenge, challenge, len)))
            return make_result(session, CP_BOOT_EV_AUTH_PENDING, CP_BOOT_OK, 0u);
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED, CP_BOOT_ERR_ORDER,
                            &result);
        return result;
    }
    if (len > sizeof(session->auth_request.challenge) ||
        session->next_auth_token == UINT64_MAX ||
        session->last_now_ms > UINT64_MAX - CP_BOOT_ASYNC_AUTH_TIMEOUT_MS) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    session->auth_request.op = op;
    session->auth_request.generation = session->generation;
    session->auth_request.token = ++session->next_auth_token;
    session->auth_request.deadline_ms = session->last_now_ms +
                                       CP_BOOT_ASYNC_AUTH_TIMEOUT_MS;
    session->auth_request.challenge_len = len;
    if (len) memcpy(session->auth_request.challenge, challenge, len);
    session->auth_exchanges++;
    return make_result(session, CP_BOOT_EV_AUTH_PENDING, CP_BOOT_OK, 0u);
}

cp_boot_result cp_bootstrap_auth_complete(
    cp_bootstrap *session, uint64_t generation, uint64_t token,
    cp_boot_auth_op op, bool success, const uint8_t *blob, size_t blob_len,
    uint64_t now_ms, uint8_t *out, size_t out_cap)
{
    cp_boot_auth_request request;
    cp_boot_result result;
    size_t out_len = 0u;
    cp_wire_status status;
    if (!cp_bootstrap_auth_request(session, &request) ||
        generation != request.generation || token != request.token ||
        op != request.op)
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ORDER, 0u);
    if (!accept_time(session, now_ms))
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    if (now_ms >= request.deadline_ms || !success || blob == NULL ||
        blob_len == 0u || blob_len > CP_BOOT_MAX_AUTH_BLOB) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            now_ms >= request.deadline_ms ? CP_BOOT_ERR_TIMEOUT :
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    status = op == CP_BOOT_AUTH_OP_CERTIFICATE ?
        cp_wire_encode_auth_certificate(blob, blob_len, out, out_cap, &out_len) :
        cp_wire_encode_auth_response(blob, blob_len, out, out_cap, &out_len);
    if (status == CP_WIRE_ERR_BUFFER || status == CP_WIRE_ERR_ARGUMENT)
        return error_result(session, CP_BOOT_ERR_BUFFER);
    if (status != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED, CP_BOOT_ERR_WIRE,
                            &result);
        return result;
    }
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    session->auth_state = op == CP_BOOT_AUTH_OP_CERTIFICATE ?
        CP_BOOT_AUTH_CERT_SENT : CP_BOOT_AUTH_RESPONSE_SENT;
    return make_result(session, op == CP_BOOT_AUTH_OP_CERTIFICATE ?
                       CP_BOOT_EV_AUTH_CERT_SENT : CP_BOOT_EV_AUTH_RESPONSE_SENT,
                       CP_BOOT_OK, out_len);
}

static bool id_equal_ci(const char *a, const char *b)
{
    size_t i = 0;

    if (a == NULL || b == NULL) {
        return false;
    }
    while (a[i] != '\0' && b[i] != '\0') {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static bool copy_id(char *dst, size_t cap, const char *src)
{
    size_t n = bounded_len(src, cap - 1u);

    if (dst == NULL || cap == 0u || src == NULL || n >= cap) {
        return false;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
    return true;
}

static const char *effective_ssid(const cp_bootstrap *session);

static cp_wire_status encode_identification(const cp_bootstrap *session,
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len)
{
    cp_wire_identification id;
    const char *languages[CP_BOOT_MAX_LANGUAGES];
    size_t i;

    memset(&id, 0, sizeof(id));
    for (i = 0; i < session->supported_language_count; i++) {
        languages[i] = session->supported_languages[i];
    }
    id.name = session->name;
    id.model_identifier = session->model_identifier;
    id.manufacturer = session->manufacturer;
    id.serial_number = session->serial_number;
    id.firmware_version = session->firmware_version;
    id.hardware_version = session->hardware_version;
    id.eap_name = session->eap_name;
    id.current_language = session->current_language;
    id.supported_languages = languages;
    id.supported_language_count = session->supported_language_count;
    id.app_match_team_id =
        (!session->drop_app_match_team_id &&
         session->app_match_team_id[0] != '\0')
            ? session->app_match_team_id
            : NULL;
    id.bluetooth_transport_name = session->bluetooth_transport_name;
    memcpy(id.bluetooth_mac, session->bluetooth_mac, 6u);
    /* Identification and start-session must name the network actually on
     * air when readback is available. */
    id.wireless_transport_name = effective_ssid(session);
    id.maximum_current_drawn_ma = session->maximum_current_drawn_ma;
    return cp_wire_encode_identification(&id, out, out_cap, out_len);
}

static cp_boot_result send_identification(cp_bootstrap *session, uint8_t *out,
                                          size_t out_cap)
{
    cp_wire_status st;
    size_t out_len = 0u;

    if (session->identification_sends >= CP_BOOT_MAX_IDENT_SENDS) {
        cp_boot_result result;
        fail_identification(session, CP_BOOT_EV_IDENTIFICATION_REJECTED,
                            CP_BOOT_ERR_IDENTIFICATION, &result);
        return result;
    }
    st = encode_identification(session, out, out_cap, &out_len);
    if (st == CP_WIRE_ERR_BUFFER) {
        return error_result(session, CP_BOOT_ERR_BUFFER);
    }
    if (st != CP_WIRE_OK) {
        session->state = CP_BOOT_STATE_FAILED;
        session->identification_sent = false;
        session->identified = false;
        session->auth_state = CP_BOOT_AUTH_NONE;
        session->auth_exchanges = 0u;
        clear_pending(session);
        clear_offer(session);
        session->generation++;
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    session->identification_sends++;
    session->identification_sent = true;
    session->state = CP_BOOT_STATE_IDENTIFYING;
    return make_result(session, CP_BOOT_EV_IDENTIFICATION_SENT, CP_BOOT_OK,
                       out_len);
}

static const char *effective_ssid(const cp_bootstrap *session)
{
    if (session->readback_valid) {
        return session->readback_ssid;
    }
    return session->ap_ssid;
}

static uint8_t effective_channel(const cp_bootstrap *session)
{
    if (session->readback_valid) {
        return session->readback_channel;
    }
    return session->ap_channel;
}

static cp_wire_wifi_security effective_security(const cp_bootstrap *session)
{
    if (session->readback_valid) {
        return session->readback_security;
    }
    return session->ap_security;
}

static const char *effective_link_local(const cp_bootstrap *session)
{
    if (session->readback_valid && session->readback_link_local[0] != '\0') {
        return session->readback_link_local;
    }
    return session->link_local;
}

static cp_wire_status encode_start(const cp_bootstrap *session, uint8_t *out,
                                   size_t out_cap, size_t *out_len)
{
    cp_wire_start_session start;

    memset(&start, 0, sizeof(start));
    start.ssid = effective_ssid(session);
    start.passphrase = session->ap_passphrase;
    start.channel = effective_channel(session);
    start.security = effective_security(session);
    start.link_local = effective_link_local(session);
    start.port = session->airplay_port;
    start.device_identifier = session->device_identifier;
    start.public_key =
        session->public_key[0] != '\0' ? session->public_key : NULL;
    start.source_version =
        session->source_version[0] != '\0' ? session->source_version : NULL;
    return cp_wire_encode_start_session(&start, out, out_cap, out_len);
}

static cp_boot_consent query_consent(cp_bootstrap *session, uint64_t now_ms)
{
    if (session->consent == NULL) {
        return CP_BOOT_CONSENT_DENIED;
    }
    return session->consent(session->policy_context, now_ms);
}

static bool query_usb_active(cp_bootstrap *session)
{
    if (session->usb_active == NULL) {
        return false;
    }
    return session->usb_active(session->policy_context,
                               session->have_phone_bluetooth_id
                                   ? session->phone_bluetooth_id
                                   : "",
                               session->have_phone_usb_id
                                   ? session->phone_usb_id
                                   : "");
}

static void arm_pending(cp_bootstrap *session, uint64_t now_ms)
{
    session->pending_start = true;
    session->pending_deadline_ms =
        deadline_after(now_ms, session->ap_wait_ms);
    session->pending_generation = session->generation;
}

static cp_boot_result attempt_start(cp_bootstrap *session, uint64_t now_ms,
                                    uint8_t *out, size_t out_cap)
{
    cp_boot_consent consent;
    cp_wire_status st;
    size_t out_len = 0u;

    if (session->pending_start) {
        if (session->pending_generation != session->generation) {
            clear_pending(session);
            return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
        }
        if (now_ms >= session->pending_deadline_ms) {
            clear_pending(session);
            session->generation++;
            reflect_invalidated_state(session);
            return make_result(session, CP_BOOT_EV_START_TIMEOUT,
                               CP_BOOT_ERR_TIMEOUT, 0u);
        }
    }
    if (session->state == CP_BOOT_STATE_FAILED) {
        /* Defense in depth: a failed identification/authentication state can
         * never emit a start, even if another field were stale. */
        clear_pending(session);
        clear_offer(session);
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    if (session->auth_state != CP_BOOT_AUTH_SUCCEEDED ||
        !session->identified) {
        clear_pending(session);
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    if (session->wireless_update_known &&
        !session->wireless_update_available) {
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    consent = query_consent(session, now_ms);
    if (consent == CP_BOOT_CONSENT_PENDING) {
        return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
    }
    if (consent != CP_BOOT_CONSENT_GRANTED) {
        /* Denied, missing or an unexpected provider value fails closed. */
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_CONSENT_DENIED,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    if (query_usb_active(session)) {
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    if (session->ap_state != CP_BOOT_AP_READY || !session->readback_valid) {
        return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
    }
    st = encode_start(session, out, out_cap, &out_len);
    if (st == CP_WIRE_ERR_BUFFER || st == CP_WIRE_ERR_ARGUMENT) {
        return error_result(session, CP_BOOT_ERR_BUFFER);
    }
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    clear_pending(session);
    session->offer_active = true;
    session->offer_held = false;
    session->offer_hold_generation = 0u;
    session->offer_deadline_ms = deadline_after(now_ms, session->ap_wait_ms);
    session->state = CP_BOOT_STATE_AWAITING_RECEIVER;
    return make_result(session, CP_BOOT_EV_START_SENT, CP_BOOT_OK, out_len);
}

static bool frame_is_accessory_message(uint16_t msg_id)
{
    return msg_id == CP_WIRE_MSG_IDENTIFICATION_INFO ||
           msg_id == CP_WIRE_MSG_AUTH_CERTIFICATE ||
           msg_id == CP_WIRE_MSG_AUTH_RESPONSE ||
           msg_id == CP_WIRE_MSG_WIFI_CONFIG_INFO ||
           msg_id == CP_WIRE_MSG_CARPLAY_START;
}

static cp_boot_result handle_identification_start(cp_bootstrap *session,
                                                  const cp_wire_view *view,
                                                  uint8_t *out,
                                                  size_t out_cap)
{
    cp_wire_status st;

    if (session->state == CP_BOOT_STATE_FAILED) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    st = cp_wire_decode_empty(view, CP_WIRE_MSG_IDENTIFICATION_START);
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (session->identified) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    return send_identification(session, out, out_cap);
}

static cp_boot_result handle_identification_accepted(cp_bootstrap *session,
                                                     const cp_wire_view *view)
{
    cp_wire_status st;

    /* Accept only while identification is outstanding.  In particular a
     * FAILED session must not be resurrected by a late 1D02. */
    if (session->state != CP_BOOT_STATE_IDENTIFYING ||
        !session->identification_sent || session->identified) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    st = cp_wire_decode_empty(view, CP_WIRE_MSG_IDENTIFICATION_ACCEPTED);
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    session->identified = true;
    session->state = CP_BOOT_STATE_IDENTIFIED;
    return make_result(session, CP_BOOT_EV_IDENTIFIED, CP_BOOT_OK, 0u);
}

static cp_boot_result handle_identification_rejected(cp_bootstrap *session,
                                                     const cp_wire_view *view,
                                                     uint8_t *out,
                                                     size_t out_cap)
{
    cp_wire_identification_rejection rejection;
    cp_wire_status st;
    bool only_team_id;
    cp_boot_result result;

    /* A rejection is meaningful only while identification is outstanding.
     * Late or post-authentication rejects are ignored before decoding: a
     * malformed late frame must not fail a session that already succeeded. */
    if (session->state != CP_BOOT_STATE_IDENTIFYING ||
        !session->identification_sent || session->identified) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    st = cp_wire_decode_identification_rejection(view, &rejection);
    if (st != CP_WIRE_OK) {
        fail_identification(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_WIRE,
                            &result);
        return result;
    }
    if (rejection.flagged == 0u) {
        /* Only unimplemented rejection flags: nonfatal, keep identifying and
         * do not discard a field this accessory actually sent. */
        return make_result(session, CP_BOOT_EV_UNSUPPORTED, CP_BOOT_OK, 0u);
    }

    only_team_id = rejection.flagged == (uint32_t)(1u << 11u);
    if (!only_team_id || session->drop_app_match_team_id ||
        session->app_match_team_id[0] == '\0' ||
        session->identification_retries >= CP_BOOT_MAX_IDENT_RETRIES) {
        fail_identification(session, CP_BOOT_EV_IDENTIFICATION_REJECTED,
                            CP_BOOT_ERR_IDENTIFICATION, &result);
        return result;
    }

    session->drop_app_match_team_id = true;
    session->identification_retries++;
    result = send_identification(session, out, out_cap);
    if (result.event == CP_BOOT_EV_IDENTIFICATION_SENT) {
        result.event = CP_BOOT_EV_IDENTIFICATION_RETRY;
    }
    return result;
}

static cp_boot_result respond_auth_certificate(cp_bootstrap *session,
                                               uint8_t *out, size_t out_cap)
{
    size_t blob_len = 0u;
    size_t out_len = 0u;
    cp_wire_status st;
    int rc;
    cp_boot_result result;

    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_FAILED) {
        return make_result(session, CP_BOOT_EV_AUTH_FAILED,
                           CP_BOOT_ERR_AUTH_FAILED, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_RESPONSE_SENT) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_ORDER, &result);
        return result;
    }
    if (session->auth_exchanges >= CP_BOOT_MAX_AUTH_EXCHANGES) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_FAILED, &result);
        return result;
    }
    if (session->async_auth)
        return begin_async_auth(session, CP_BOOT_AUTH_OP_CERTIFICATE, NULL, 0u);
    if (session->auth_provider.certificate == NULL) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    rc = session->auth_provider.certificate(
        session->auth_provider.context, session->auth_scratch,
        sizeof(session->auth_scratch), &blob_len);
    if (rc != 0 || blob_len == 0u ||
        blob_len > sizeof(session->auth_scratch)) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    session->auth_exchanges++;
    st = cp_wire_encode_auth_certificate(session->auth_scratch, blob_len, out,
                                         out_cap, &out_len);
    if (st == CP_WIRE_ERR_BUFFER || st == CP_WIRE_ERR_ARGUMENT) {
        return error_result(session, CP_BOOT_ERR_BUFFER);
    }
    if (st != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_WIRE, &result);
        return result;
    }
    session->auth_state = CP_BOOT_AUTH_CERT_SENT;
    return make_result(session, CP_BOOT_EV_AUTH_CERT_SENT, CP_BOOT_OK,
                       out_len);
}

static cp_boot_result handle_auth_certificate_request(
    cp_bootstrap *session, const cp_wire_view *view, uint8_t *out,
    size_t out_cap)
{
    cp_wire_status st;

    if (session->auth_state == CP_BOOT_AUTH_FAILED) {
        return make_result(session, CP_BOOT_EV_AUTH_FAILED,
                           CP_BOOT_ERR_AUTH_FAILED, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    st = cp_wire_decode_empty(view, CP_WIRE_MSG_AUTH_CERT_REQUEST);
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (session->state == CP_BOOT_STATE_FAILED) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    if (!session->identified) {
        /* Auth-first order: decode and buffer, but never call a credential
         * provider or emit AA01 before identification succeeds. */
        session->pending_auth_cert = true;
        return make_result(session, CP_BOOT_EV_AUTH_PENDING, CP_BOOT_OK, 0u);
    }
    return respond_auth_certificate(session, out, out_cap);
}

static cp_boot_result respond_auth_challenge(cp_bootstrap *session,
                                             const uint8_t *challenge,
                                             size_t challenge_len,
                                             uint8_t *out, size_t out_cap)
{
    size_t blob_len = 0u;
    size_t out_len = 0u;
    cp_wire_status st;
    int rc;
    cp_boot_result result;

    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_FAILED) {
        return make_result(session, CP_BOOT_EV_AUTH_FAILED,
                           CP_BOOT_ERR_AUTH_FAILED, 0u);
    }
    if (session->auth_state != CP_BOOT_AUTH_CERT_SENT) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_ORDER, &result);
        return result;
    }
    if (session->auth_exchanges >= CP_BOOT_MAX_AUTH_EXCHANGES) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_FAILED, &result);
        return result;
    }
    if (session->async_auth)
        return begin_async_auth(session, CP_BOOT_AUTH_OP_SIGNATURE,
                                challenge, challenge_len);
    if (session->auth_provider.signature == NULL) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    rc = session->auth_provider.signature(
        session->auth_provider.context, challenge, challenge_len,
        session->auth_scratch, sizeof(session->auth_scratch), &blob_len);
    if (rc != 0 || blob_len == 0u ||
        blob_len > sizeof(session->auth_scratch)) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_AUTH_PROVIDER, &result);
        return result;
    }
    session->auth_exchanges++;
    st = cp_wire_encode_auth_response(session->auth_scratch, blob_len, out,
                                      out_cap, &out_len);
    if (st == CP_WIRE_ERR_BUFFER || st == CP_WIRE_ERR_ARGUMENT) {
        return error_result(session, CP_BOOT_ERR_BUFFER);
    }
    if (st != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_WIRE, &result);
        return result;
    }
    session->auth_state = CP_BOOT_AUTH_RESPONSE_SENT;
    return make_result(session, CP_BOOT_EV_AUTH_RESPONSE_SENT, CP_BOOT_OK,
                       out_len);
}

static cp_boot_result handle_auth_challenge_request(cp_bootstrap *session,
                                                    const cp_wire_view *view,
                                                    uint8_t *out,
                                                    size_t out_cap)
{
    const uint8_t *challenge = NULL;
    size_t challenge_len = 0u;
    cp_wire_status st;
    cp_boot_result result;

    if (session->auth_state == CP_BOOT_AUTH_FAILED) {
        return make_result(session, CP_BOOT_EV_AUTH_FAILED,
                           CP_BOOT_ERR_AUTH_FAILED, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    st = cp_wire_decode_challenge_request(view, &challenge, &challenge_len);
    if (st != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_WIRE, &result);
        return result;
    }
    if (session->state == CP_BOOT_STATE_FAILED) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    if (!session->identified) {
        /* Buffer one AA02.  It is answered only after identification and the
         * certificate response; a different challenge is an order error. */
        if (session->have_pending_auth_challenge) {
            if (session->pending_auth_challenge_len != challenge_len ||
                memcmp(session->pending_auth_challenge, challenge,
                       challenge_len) != 0) {
                fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                                    CP_BOOT_ERR_ORDER, &result);
                return result;
            }
            return make_result(session, CP_BOOT_EV_AUTH_PENDING, CP_BOOT_OK,
                               0u);
        }
        if (challenge_len > sizeof(session->pending_auth_challenge)) {
            fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                                CP_BOOT_ERR_AUTH_PROVIDER, &result);
            return result;
        }
        memcpy(session->pending_auth_challenge, challenge, challenge_len);
        session->pending_auth_challenge_len = challenge_len;
        session->have_pending_auth_challenge = true;
        return make_result(session, CP_BOOT_EV_AUTH_PENDING, CP_BOOT_OK, 0u);
    }
    return respond_auth_challenge(session, challenge, challenge_len, out,
                                  out_cap);
}

static cp_boot_result handle_auth_succeeded(cp_bootstrap *session,
                                            const cp_wire_view *view)
{
    cp_wire_status st;
    cp_boot_result result;

    if (session->auth_state == CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_IGNORED, CP_BOOT_OK, 0u);
    }
    if (session->auth_state == CP_BOOT_AUTH_FAILED) {
        return make_result(session, CP_BOOT_EV_AUTH_FAILED,
                           CP_BOOT_ERR_AUTH_FAILED, 0u);
    }
    if (session->auth_state != CP_BOOT_AUTH_RESPONSE_SENT ||
        !session->identified) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_ORDER, &result);
        return result;
    }
    st = cp_wire_decode_empty(view, CP_WIRE_MSG_AUTH_SUCCEEDED);
    if (st != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_WIRE, &result);
        return result;
    }
    session->auth_state = CP_BOOT_AUTH_SUCCEEDED;
    session->state = CP_BOOT_STATE_AUTHENTICATED;
    return make_result(session, CP_BOOT_EV_AUTHENTICATED, CP_BOOT_OK, 0u);
}

static cp_boot_result handle_auth_failed(cp_bootstrap *session,
                                         const cp_wire_view *view)
{
    cp_wire_status st = cp_wire_decode_empty(view, CP_WIRE_MSG_AUTH_FAILED);
    cp_boot_result result;

    if (st != CP_WIRE_OK) {
        fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                            CP_BOOT_ERR_WIRE, &result);
        return result;
    }
    fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                        CP_BOOT_ERR_AUTH_FAILED, &result);
    return result;
}

static cp_boot_result handle_wifi_config_request(cp_bootstrap *session,
                                                 const cp_wire_view *view,
                                                 uint8_t *out,
                                                 size_t out_cap)
{
    cp_wire_wifi_config cfg;
    cp_wire_status st;
    size_t out_len = 0u;

    st = cp_wire_decode_empty(view, CP_WIRE_MSG_WIFI_CONFIG_REQUEST);
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (session->auth_state != CP_BOOT_AUTH_SUCCEEDED ||
        !session->identified) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    cfg.ssid = effective_ssid(session);
    cfg.passphrase = session->ap_passphrase;
    cfg.channel = effective_channel(session);
    st = cp_wire_encode_wifi_config(&cfg, out, out_cap, &out_len);
    if (st == CP_WIRE_ERR_BUFFER || st == CP_WIRE_ERR_ARGUMENT) {
        return error_result(session, CP_BOOT_ERR_BUFFER);
    }
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    return make_result(session, CP_BOOT_EV_WIFI_CONFIG_SENT, CP_BOOT_OK,
                       out_len);
}

static cp_boot_result handle_wireless_update(cp_bootstrap *session,
                                             const cp_wire_view *view)
{
    cp_wire_wireless_update update;
    cp_wire_status st = cp_wire_decode_wireless_update(view, &update);

    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (!session->identified) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    session->wireless_update_known = true;
    session->wireless_update_available = update.available;
    if (!update.available) {
        if (session->pending_start || session->offer_active) {
            invalidate_offer(session);
        }
    }
    return make_result(session, CP_BOOT_EV_WIRELESS_STATUS, CP_BOOT_OK, 0u);
}

static cp_boot_result handle_transport_ids(cp_bootstrap *session,
                                           const cp_wire_view *view)
{
    cp_wire_transport_ids ids;
    cp_wire_status st = cp_wire_decode_transport_ids(view, &ids);
    bool changed_identity = false;

    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (!session->identified) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    if (ids.bluetooth_transport_id[0] != '\0') {
        if (session->have_phone_bluetooth_id &&
            !id_equal_ci(session->phone_bluetooth_id,
                         ids.bluetooth_transport_id)) {
            changed_identity = true;
        }
        if (!copy_id(session->phone_bluetooth_id,
                     sizeof(session->phone_bluetooth_id),
                     ids.bluetooth_transport_id)) {
            return error_result(session, CP_BOOT_ERR_WIRE);
        }
        session->have_phone_bluetooth_id = true;
    }
    if (ids.usb_transport_id[0] != '\0') {
        if (!session->have_phone_usb_id ||
            !id_equal_ci(session->phone_usb_id, ids.usb_transport_id)) {
            changed_identity = true;
        }
        if (!copy_id(session->phone_usb_id, sizeof(session->phone_usb_id),
                     ids.usb_transport_id)) {
            return error_result(session, CP_BOOT_ERR_WIRE);
        }
        session->have_phone_usb_id = true;
    }
    if (changed_identity && (session->pending_start || session->offer_active)) {
        invalidate_offer(session);
    }
    return make_result(session, CP_BOOT_EV_PHONE_IDENTITY, CP_BOOT_OK, 0u);
}

static cp_boot_result handle_availability(cp_bootstrap *session,
                                          const cp_wire_view *view,
                                          uint64_t now_ms, uint8_t *out,
                                          size_t out_cap)
{
    cp_wire_availability availability;
    cp_wire_status st = cp_wire_decode_availability(view, &availability);
    cp_boot_consent consent;

    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (session->auth_state != CP_BOOT_AUTH_SUCCEEDED) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }

    if (session->offer_held &&
        session->offer_hold_generation != session->generation) {
        /* Defensive: a generation change always clears the hold, but never let
         * a stale hold retire a newer offer's deadline. */
        session->offer_held = false;
    }

    /* Conservative availability policy: the wireless side must be present and
     * explicitly available.  A missing optional field is not assumed true. */
    if (!availability.wireless.present ||
        !availability.wireless.available_known ||
        !availability.wireless.available) {
        if (session->pending_start || session->offer_active) {
            invalidate_offer(session);
        }
        return make_result(session, CP_BOOT_EV_AVAILABILITY_FALSE, CP_BOOT_OK,
                           0u);
    }

    if (availability.wireless.transport_id[0] != '\0') {
        if (session->have_phone_bluetooth_id &&
            !id_equal_ci(session->phone_bluetooth_id,
                         availability.wireless.transport_id)) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_REFUSED,
                               CP_BOOT_ERR_IDENTITY, 0u);
        }
        if (!session->have_phone_bluetooth_id) {
            if (!copy_id(session->phone_bluetooth_id,
                         sizeof(session->phone_bluetooth_id),
                         availability.wireless.transport_id)) {
                return error_result(session, CP_BOOT_ERR_WIRE);
            }
            session->have_phone_bluetooth_id = true;
        }
    }
    if (availability.wired.transport_id[0] != '\0') {
        if (session->have_phone_usb_id &&
            !id_equal_ci(session->phone_usb_id,
                         availability.wired.transport_id)) {
            if (!copy_id(session->phone_usb_id,
                         sizeof(session->phone_usb_id),
                         availability.wired.transport_id)) {
                return error_result(session, CP_BOOT_ERR_WIRE);
            }
            /* While a trusted hold is in place, propagate the new wired id
             * before evaluating policy: the USB callback must see the id it is
             * deciding about.  A non-held offer keeps the conservative
             * credential-change refusal. */
            if ((session->pending_start || session->offer_active) &&
                !session->offer_held) {
                invalidate_offer(session);
                return make_result(session, CP_BOOT_EV_START_REFUSED,
                                   CP_BOOT_ERR_IDENTITY, 0u);
            }
        } else if (!session->have_phone_usb_id) {
            if (!copy_id(session->phone_usb_id,
                         sizeof(session->phone_usb_id),
                         availability.wired.transport_id)) {
                return error_result(session, CP_BOOT_ERR_WIRE);
            }
            session->have_phone_usb_id = true;
        }
    }

    if (session->offer_held && session->offer_active) {
        consent = query_consent(session, now_ms);
        /* A trusted observation retires only the deadline.  It does not hide
         * policy or identity changes: availability has already been decoded
         * and the ids above updated, so the USB callback decides with the id
         * the phone just reported.  USB/consent are re-queried here, before a
         * duplicate is returned, instead of waiting for the next bounded
         * re-check. */
        if (session->state == CP_BOOT_STATE_FAILED ||
            session->auth_state != CP_BOOT_AUTH_SUCCEEDED) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_REFUSED,
                               CP_BOOT_ERR_ORDER, 0u);
        }
        if (session->wireless_update_known &&
            !session->wireless_update_available) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_REFUSED,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        if (availability.wired.present && availability.wired.available_known &&
            availability.wired.available && session->usb_active == NULL) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        if (query_usb_active(session)) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        if (consent != CP_BOOT_CONSENT_GRANTED &&
            consent != CP_BOOT_CONSENT_PENDING) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_CONSENT_DENIED,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        return make_result(session, CP_BOOT_EV_START_DUPLICATE, CP_BOOT_OK,
                           0u);
    }

    if (session->offer_active && now_ms >= session->offer_deadline_ms) {
        /* A fresh availability message may replace an offer whose bounded
         * receiver wait already elapsed, even if no tick was delivered. */
        invalidate_offer(session);
    }
    if (session->offer_active) {
        return make_result(session, CP_BOOT_EV_START_DUPLICATE, CP_BOOT_OK,
                           0u);
    }
    if (session->pending_start) {
        if (session->pending_generation != session->generation) {
            clear_pending(session);
        } else if (now_ms >= session->pending_deadline_ms) {
            clear_pending(session);
            session->generation++;
            reflect_invalidated_state(session);
            return make_result(session, CP_BOOT_EV_START_TIMEOUT,
                               CP_BOOT_ERR_TIMEOUT, 0u);
        } else {
            return make_result(session, CP_BOOT_EV_START_PENDING_AP,
                               CP_BOOT_OK, 0u);
        }
    }

    if (session->wireless_update_known &&
        !session->wireless_update_available) {
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    if (availability.wired.present && availability.wired.available_known &&
        availability.wired.available && session->usb_active == NULL) {
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    if (query_usb_active(session)) {
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                           CP_BOOT_ERR_POLICY, 0u);
    }

    consent = query_consent(session, now_ms);
    if (consent != CP_BOOT_CONSENT_GRANTED &&
        consent != CP_BOOT_CONSENT_PENDING) {
        /* Denied, missing or an unexpected provider value fails closed. */
        clear_pending(session);
        session->generation++;
        reflect_invalidated_state(session);
        return make_result(session, CP_BOOT_EV_CONSENT_DENIED,
                           CP_BOOT_ERR_POLICY, 0u);
    }
    if (consent == CP_BOOT_CONSENT_PENDING ||
        session->ap_state != CP_BOOT_AP_READY ||
        !session->readback_valid) {
        arm_pending(session, now_ms);
        return make_result(session, CP_BOOT_EV_START_PENDING_AP, CP_BOOT_OK,
                           0u);
    }
    return attempt_start(session, now_ms, out, out_cap);
}

cp_boot_result cp_bootstrap_receive(cp_bootstrap *session,
                                    const uint8_t *frame, size_t frame_len,
                                    uint64_t now_ms, uint8_t *out,
                                    size_t out_cap)
{
    cp_wire_view view;
    cp_wire_status st;

    if (session == NULL || frame == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    st = cp_wire_open(frame, frame_len, &view);
    if (st != CP_WIRE_OK) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }
    if (frame_is_accessory_message(view.msg_id)) {
        return error_result(session, CP_BOOT_ERR_WIRE);
    }

    switch (view.msg_id) {
    case CP_WIRE_MSG_IDENTIFICATION_START:
        return handle_identification_start(session, &view, out, out_cap);
    case CP_WIRE_MSG_IDENTIFICATION_ACCEPTED:
        return handle_identification_accepted(session, &view);
    case CP_WIRE_MSG_IDENTIFICATION_REJECTED:
        return handle_identification_rejected(session, &view, out, out_cap);
    case CP_WIRE_MSG_AUTH_CERT_REQUEST:
        return handle_auth_certificate_request(session, &view, out, out_cap);
    case CP_WIRE_MSG_AUTH_CHALLENGE_REQUEST:
        return handle_auth_challenge_request(session, &view, out, out_cap);
    case CP_WIRE_MSG_AUTH_SUCCEEDED:
        return handle_auth_succeeded(session, &view);
    case CP_WIRE_MSG_AUTH_FAILED:
        return handle_auth_failed(session, &view);
    case CP_WIRE_MSG_WIFI_CONFIG_REQUEST:
        return handle_wifi_config_request(session, &view, out, out_cap);
    case CP_WIRE_MSG_WIRELESS_UPDATE:
        return handle_wireless_update(session, &view);
    case CP_WIRE_MSG_TRANSPORT_IDS:
        return handle_transport_ids(session, &view);
    case CP_WIRE_MSG_CARPLAY_AVAILABILITY:
        return handle_availability(session, &view, now_ms, out, out_cap);
    default:
        return make_result(session, CP_BOOT_EV_UNSUPPORTED, CP_BOOT_OK, 0u);
    }
}

cp_boot_result cp_bootstrap_tick(cp_bootstrap *session, uint64_t now_ms,
                                 uint8_t *out, size_t out_cap)
{
    cp_boot_result result;

    if (session == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    if (session->auth_request.op != CP_BOOT_AUTH_OP_NONE) {
        if (session->auth_request.generation != session->generation) {
            memset(&session->auth_request, 0, sizeof(session->auth_request));
        } else if (now_ms >= session->auth_request.deadline_ms) {
            fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                                CP_BOOT_ERR_TIMEOUT, &result);
            return result;
        }
    }
    /* Service auth that arrived before identification.  Nothing is released
     * until identified is true; AA05 remains a separate phone decision. */
    if (session->identified && session->state != CP_BOOT_STATE_FAILED) {
        if (session->pending_auth_cert &&
            session->auth_state == CP_BOOT_AUTH_NONE) {
            session->pending_auth_cert = false;
            return respond_auth_certificate(session, out, out_cap);
        }
        if (session->have_pending_auth_challenge &&
            session->auth_state == CP_BOOT_AUTH_CERT_SENT) {
            uint8_t challenge[CP_BOOT_MAX_AUTH_BLOB];
            size_t challenge_len = session->pending_auth_challenge_len;
            if (challenge_len > sizeof(challenge)) {
                fail_authentication(session, CP_BOOT_EV_AUTH_FAILED,
                                    CP_BOOT_ERR_AUTH_PROVIDER, &result);
                return result;
            }
            memcpy(challenge, session->pending_auth_challenge, challenge_len);
            session->have_pending_auth_challenge = false;
            session->pending_auth_challenge_len = 0u;
            return respond_auth_challenge(session, challenge, challenge_len,
                                          out, out_cap);
        }
    }
    if (session->pending_start) {
        if (session->pending_generation != session->generation) {
            clear_pending(session);
            return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
        }
        if (now_ms >= session->pending_deadline_ms) {
            clear_pending(session);
            session->generation++;
            reflect_invalidated_state(session);
            return make_result(session, CP_BOOT_EV_START_TIMEOUT,
                               CP_BOOT_ERR_TIMEOUT, 0u);
        }
        result = attempt_start(session, now_ms, out, out_cap);
        if (result.event == CP_BOOT_EV_NONE && !session->pending_start) {
            /* attempt_start only clears pending on a terminal policy result. */
            reflect_invalidated_state(session);
        }
        return result;
    }
    if (session->offer_active) {
        cp_boot_consent consent;

        if (session->offer_held &&
            session->offer_hold_generation != session->generation)
            session->offer_held = false;
        if (session->offer_held) {
            if (session->state == CP_BOOT_STATE_FAILED ||
                session->auth_state != CP_BOOT_AUTH_SUCCEEDED) {
                invalidate_offer(session);
                return make_result(session, CP_BOOT_EV_START_REFUSED,
                                   CP_BOOT_ERR_ORDER, 0u);
            }
            if (session->wireless_update_known &&
                !session->wireless_update_available) {
                invalidate_offer(session);
                return make_result(session, CP_BOOT_EV_START_REFUSED,
                                   CP_BOOT_ERR_POLICY, 0u);
            }
            consent = query_consent(session, now_ms);
            if (consent != CP_BOOT_CONSENT_GRANTED &&
                consent != CP_BOOT_CONSENT_PENDING) {
                invalidate_offer(session);
                return make_result(session, CP_BOOT_EV_CONSENT_DENIED,
                                   CP_BOOT_ERR_POLICY, 0u);
            }
            if (query_usb_active(session)) {
                invalidate_offer(session);
                return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                                   CP_BOOT_ERR_POLICY, 0u);
            }
            return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
        }
        if (now_ms >= session->offer_deadline_ms) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_TIMEOUT,
                               CP_BOOT_ERR_TIMEOUT, 0u);
        }
        if (session->state == CP_BOOT_STATE_FAILED ||
            session->auth_state != CP_BOOT_AUTH_SUCCEEDED) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_REFUSED,
                               CP_BOOT_ERR_ORDER, 0u);
        }
        if (session->wireless_update_known &&
            !session->wireless_update_available) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_START_REFUSED,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        consent = query_consent(session, now_ms);
        if (consent != CP_BOOT_CONSENT_GRANTED &&
            consent != CP_BOOT_CONSENT_PENDING) {
            /* Denied, missing or an unexpected provider value fails closed. */
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_CONSENT_DENIED,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        if (query_usb_active(session)) {
            invalidate_offer(session);
            return make_result(session, CP_BOOT_EV_USB_ACTIVE,
                               CP_BOOT_ERR_POLICY, 0u);
        }
        return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
    }
    return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
}


cp_boot_result cp_bootstrap_hold_offer(cp_bootstrap *session,
                                       uint64_t generation, uint64_t now_ms)
{
    if (session == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    if (generation != session->generation || !session->offer_active ||
        session->state != CP_BOOT_STATE_AWAITING_RECEIVER ||
        session->auth_state != CP_BOOT_AUTH_SUCCEEDED ||
        !session->identified) {
        return make_result(session, CP_BOOT_EV_START_REFUSED,
                           CP_BOOT_ERR_ORDER, 0u);
    }
    session->offer_held = true;
    session->offer_hold_generation = session->generation;
    return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
}

cp_boot_result cp_bootstrap_release_offer_hold(cp_bootstrap *session,
                                               uint64_t now_ms)
{
    if (session == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    session->offer_held = false;
    session->offer_hold_generation = 0u;
    return make_result(session, CP_BOOT_EV_NONE, CP_BOOT_OK, 0u);
}

bool cp_bootstrap_offer_held(const cp_bootstrap *session)
{
    return session != NULL && session->offer_active &&
           session->offer_held &&
           session->offer_hold_generation == session->generation;
}

cp_boot_result cp_bootstrap_set_ap_readback(cp_bootstrap *session,
                                            const cp_boot_ap_readback *readback,
                                            uint64_t now_ms, uint8_t *out,
                                            size_t out_cap)
{
    char ssid[CP_WIRE_MAX_SSID + 1u];
    char link_local[CP_WIRE_MAX_LINK_LOCAL + 1u];

    if (session == NULL || readback == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    if (!readback->on_air) {
        /* AP-scope invalidation: the trusted offer hold cannot outlive the AP
         * configuration it was granted for, but the bounded offer itself is
         * left to its normal deadline/cancel path. */
        session->offer_held = false;
        session->offer_hold_generation = 0u;
        session->ap_state = CP_BOOT_AP_NOT_READY;
        session->readback_valid = false;
        session->readback_ssid[0] = '\0';
        session->readback_link_local[0] = '\0';
        session->readback_channel = 0u;
        return make_result(session, CP_BOOT_EV_AP_NOT_READY, CP_BOOT_OK, 0u);
    }
    if (readback->ssid == NULL || readback->channel == 0u ||
        readback->channel > 233u ||
        validate_security(readback->security) != CP_BOOT_OK) {
        return error_result(session, CP_BOOT_ERR_CONFIG);
    }
    memset(ssid, 0, sizeof(ssid));
    memset(link_local, 0, sizeof(link_local));
    if (copy_text(ssid, sizeof(ssid), readback->ssid, 1u,
                  CP_WIRE_MAX_SSID) != CP_BOOT_OK) {
        return error_result(session, CP_BOOT_ERR_CONFIG);
    }
    if (readback->link_local != NULL && readback->link_local[0] != '\0') {
        if (!cp_wire_link_local_usable(readback->link_local)) {
            return error_result(session, CP_BOOT_ERR_CONFIG);
        }
        if (copy_text(link_local, sizeof(link_local), readback->link_local,
                      1u, CP_WIRE_MAX_LINK_LOCAL) != CP_BOOT_OK) {
            return error_result(session, CP_BOOT_ERR_CONFIG);
        }
    }
    {
        /* A valid but different on-air readback is a new AP scope, exactly
         * like an off-air readback.  The hold was granted for the old scope
         * and must not survive the change; the bounded offer, its original
         * deadline and the generation are deliberately left untouched. */
        bool ap_scope_changed =
            !session->readback_valid ||
            session->readback_channel != readback->channel ||
            session->readback_security != readback->security ||
            strcmp(session->readback_ssid, ssid) != 0 ||
            strcmp(session->readback_link_local, link_local) != 0;

        if (ap_scope_changed) {
            session->offer_held = false;
            session->offer_hold_generation = 0u;
        }
    }
    memcpy(session->readback_ssid, ssid, sizeof(ssid));
    memcpy(session->readback_link_local, link_local, sizeof(link_local));
    session->readback_channel = readback->channel;
    session->readback_security = readback->security;
    session->readback_valid = true;
    session->ap_state = CP_BOOT_AP_READY;

    if (session->pending_start) {
        return attempt_start(session, now_ms, out, out_cap);
    }
    return make_result(session, CP_BOOT_EV_AP_READY, CP_BOOT_OK, 0u);
}

cp_boot_result cp_bootstrap_cancel(cp_bootstrap *session, uint64_t now_ms)
{
    if (session == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    clear_pending_auth(session);
    clear_pending(session);
    clear_offer(session);
    session->generation++;
    if (session->state == CP_BOOT_STATE_AWAITING_RECEIVER) {
        reflect_invalidated_state(session);
    }
    return make_result(session, CP_BOOT_EV_CANCELLED, CP_BOOT_OK, 0u);
}

cp_boot_result cp_bootstrap_transport_reset(cp_bootstrap *session,
                                           uint64_t now_ms)
{
    if (session == NULL) {
        return make_result(session, CP_BOOT_EV_ERROR, CP_BOOT_ERR_ARGUMENT,
                           0u);
    }
    if (!accept_time(session, now_ms)) {
        return error_result(session, CP_BOOT_ERR_ARGUMENT);
    }
    memset(&session->auth_request, 0, sizeof(session->auth_request));
    clear_pending_auth(session);
    clear_pending(session);
    clear_offer(session);
    session->generation++;
    session->state = CP_BOOT_STATE_DISCONNECTED;
    session->auth_state = CP_BOOT_AUTH_NONE;
    session->identification_sent = false;
    session->identified = false;
    session->identification_sends = 0u;
    session->identification_retries = 0u;
    session->drop_app_match_team_id = false;
    session->have_phone_bluetooth_id = false;
    session->have_phone_usb_id = false;
    session->phone_bluetooth_id[0] = '\0';
    session->phone_usb_id[0] = '\0';
    session->wireless_update_known = false;
    session->wireless_update_available = false;
    session->auth_exchanges = 0u;
    session->projection_active = false;
    return make_result(session, CP_BOOT_EV_TRANSPORT_RESET, CP_BOOT_OK, 0u);
}

cp_boot_state cp_bootstrap_state(const cp_bootstrap *session)
{
    return session != NULL ? session->state : CP_BOOT_STATE_DISCONNECTED;
}

cp_boot_auth_state cp_bootstrap_auth_state(const cp_bootstrap *session)
{
    return session != NULL ? session->auth_state : CP_BOOT_AUTH_NONE;
}

cp_boot_ap_state cp_bootstrap_ap_state(const cp_bootstrap *session)
{
    return session != NULL ? session->ap_state : CP_BOOT_AP_UNKNOWN;
}

bool cp_bootstrap_projection_active(const cp_bootstrap *session)
{
    (void)session;
    return false;
}

uint64_t cp_bootstrap_generation(const cp_bootstrap *session)
{
    return session != NULL ? session->generation : 0u;
}
