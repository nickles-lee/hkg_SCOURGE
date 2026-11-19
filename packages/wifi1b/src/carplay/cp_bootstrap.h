/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded wireless CarPlay bring-up state machine for checkpoint C1.
 *
 * The state machine owns identification, authentication, WiFi configuration
 * disclosure and the bounded start-session offer.  It does not implement link
 * framing, socket I/O, AP management, discovery or projection.  All time is
 * caller-supplied monotonic milliseconds; there are no clocks, threads,
 * allocations or log statements here.
 *
 * A failed identification is terminal for the current transport association:
 * later 1D02/1D03 frames cannot revive it, and it clears the identification
 * and authentication state.  An identification rejection is accepted only
 * while the session is identifying; a late malformed rejection is ignored
 * instead of failing an already authenticated session.
 *
 * The post-start receiver offer is bounded by the same caller-supplied wait as
 * AP readiness.  Bounded ticks re-query consent and USB policy and expire the
 * offer, so a revoked policy or an absent receiver cannot leave a stale
 * "start was sent" state behind.  Projection is never reported active.
 *
 * A trusted caller that has independently observed an active receiver may
 * retire only the offer deadline with cp_bootstrap_hold_offer(), bound to the
 * current bootstrap generation.  The hold is never inferred from the wire.
 * While held, availability still reaches the normal decoder and identity
 * updates, but the offer cannot expire, re-arm or emit a second start; held
 * offers re-query USB and consent before returning a duplicate.  Withdrawal,
 * policy refusal, cancel, transport reset, failure, generation drift and
 * AP-scope invalidation (off-air, or a valid but changed SSID/channel/
 * security/link-local readback) all clear the hold, so a released observation
 * falls back to the ordinary bounded offer rather than leaving it held
 * forever.
 * Expiry alone is not evidence that an OEM receiver failed or stopped.
 *
 * Availability (0x4300) is interpreted conservatively: the wireless side must
 * be present and explicitly report availability.  A missing optional field is
 * treated as not available rather than assumed true, because the wire profile
 * does not prove that omission means availability.
 *
 * Authentication is deliberately delegated to caller-provided certificate
 * and challenge-signing callbacks.  A callback can only produce bytes; it can
 * never mark authentication successful.  Only an AA05 message from the phone,
 * after this accessory has sent both its certificate and its challenge
 * response, sets the authenticated state.
 *
 * The AP passphrase appears in 0x5703 and 0x4301 frames.  Callers must not log
 * frame bytes or configuration structures.
 *
 * A session is not internally synchronized: one caller owns it and serializes
 * receive/tick/readback/cancel/reset calls.
 */

#ifndef CP_BOOTSTRAP_H
#define CP_BOOTSTRAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One optional identification feature may be removed after a rejection.
 * The bound is fixed so a hostile or buggy peer cannot loop the accessory. */
#define CP_BOOT_MAX_IDENT_RETRIES 1u
#define CP_BOOT_MAX_IDENT_SENDS 2u
#define CP_BOOT_MAX_AUTH_EXCHANGES 8u
#define CP_BOOT_DEFAULT_AP_WAIT_MS 30000u
#define CP_BOOT_MAX_AP_WAIT_MS 600000u
#define CP_BOOT_MAX_LANGUAGES CP_WIRE_MAX_LANGUAGES
#define CP_BOOT_MAX_AUTH_BLOB CP_WIRE_MAX_AUTH_BLOB
#define CP_BOOT_MAX_TRANSPORT_ID CP_WIRE_MAX_TRANSPORT_ID
#define CP_BOOT_ASYNC_AUTH_TIMEOUT_MS 5000u

typedef enum {
    CP_BOOT_STATE_DISCONNECTED = 0,
    CP_BOOT_STATE_IDENTIFYING,
    CP_BOOT_STATE_IDENTIFIED,
    CP_BOOT_STATE_AUTHENTICATED,
    /* Start-session was sent; no receiver has been observed.  The offer is
     * bounded by ap_wait_ms and is cleared by tick expiry or a policy
     * revocation.  Projection is never reported active by this module. */
    CP_BOOT_STATE_AWAITING_RECEIVER,
    CP_BOOT_STATE_FAILED
} cp_boot_state;

typedef enum {
    CP_BOOT_AUTH_NONE = 0,
    CP_BOOT_AUTH_CERT_SENT,
    CP_BOOT_AUTH_RESPONSE_SENT,
    CP_BOOT_AUTH_SUCCEEDED,
    CP_BOOT_AUTH_FAILED
} cp_boot_auth_state;

typedef enum {
    CP_BOOT_AP_UNKNOWN = 0,
    CP_BOOT_AP_NOT_READY,
    CP_BOOT_AP_READY
} cp_boot_ap_state;

typedef enum {
    CP_BOOT_EV_NONE = 0,
    CP_BOOT_EV_IGNORED,
    CP_BOOT_EV_UNSUPPORTED,
    CP_BOOT_EV_IDENTIFICATION_SENT,
    CP_BOOT_EV_IDENTIFICATION_RETRY,
    CP_BOOT_EV_IDENTIFIED,
    CP_BOOT_EV_IDENTIFICATION_REJECTED,
    CP_BOOT_EV_AUTH_CERT_SENT,
    CP_BOOT_EV_AUTH_RESPONSE_SENT,
    CP_BOOT_EV_AUTHENTICATED,
    CP_BOOT_EV_AUTH_FAILED,
    CP_BOOT_EV_WIFI_CONFIG_SENT,
    CP_BOOT_EV_WIRELESS_STATUS,
    CP_BOOT_EV_PHONE_IDENTITY,
    /* AP, consent or USB policy is not ready yet; the offer is bounded by the
     * caller-supplied monotonic deadline. */
    CP_BOOT_EV_START_PENDING_AP,
    CP_BOOT_EV_START_SENT,
    CP_BOOT_EV_START_DUPLICATE,
    CP_BOOT_EV_START_REFUSED,
    /* The bounded AP-readiness wait or post-start receiver offer expired. */
    CP_BOOT_EV_START_TIMEOUT,
    CP_BOOT_EV_USB_ACTIVE,
    CP_BOOT_EV_CONSENT_DENIED,
    CP_BOOT_EV_AVAILABILITY_FALSE,
    CP_BOOT_EV_AP_READY,
    CP_BOOT_EV_AP_NOT_READY,
    CP_BOOT_EV_CANCELLED,
    CP_BOOT_EV_TRANSPORT_RESET,
    CP_BOOT_EV_ERROR,
    CP_BOOT_EV_AUTH_PENDING
} cp_boot_event;

typedef enum {
    CP_BOOT_OK = 0,
    CP_BOOT_ERR_ARGUMENT,
    CP_BOOT_ERR_CONFIG,
    CP_BOOT_ERR_WIRE,
    CP_BOOT_ERR_ORDER,
    CP_BOOT_ERR_IDENTIFICATION,
    CP_BOOT_ERR_AUTH_PROVIDER,
    CP_BOOT_ERR_AUTH_FAILED,
    CP_BOOT_ERR_IDENTITY,
    CP_BOOT_ERR_POLICY,
    CP_BOOT_ERR_TIMEOUT,
    CP_BOOT_ERR_BUFFER
} cp_boot_error;

typedef enum {
    CP_BOOT_CONSENT_DENIED = 0,
    CP_BOOT_CONSENT_GRANTED = 1,
    CP_BOOT_CONSENT_PENDING = 2
} cp_boot_consent;

/* Certificate and signature providers supplied by the OEM credential layer.
 * Return 0 on success.  On success out_len must be within 1..cap.  These
 * callbacks must not synthesize AA05; that decision belongs solely to the
 * phone's AuthenticationSucceeded message. */
typedef struct {
    void *context;
    int (*certificate)(void *context, uint8_t *out, size_t cap,
                       size_t *out_len);
    int (*signature)(void *context, const uint8_t *challenge,
                     size_t challenge_len, uint8_t *out, size_t cap,
                     size_t *out_len);
} cp_boot_auth_provider;

/* Opt-in deferred provider interface. A request is not authentication success.
 * The serialized caller starts a nonblocking helper operation, then supplies a
 * completion bound to both generation and monotonically increasing token. */
typedef enum {
    CP_BOOT_AUTH_OP_NONE = 0,
    CP_BOOT_AUTH_OP_CERTIFICATE,
    CP_BOOT_AUTH_OP_SIGNATURE
} cp_boot_auth_op;

typedef struct {
    cp_boot_auth_op op;
    uint64_t generation;
    uint64_t token;
    uint64_t deadline_ms;
    size_t challenge_len;
    uint8_t challenge[CP_BOOT_MAX_AUTH_BLOB];
} cp_boot_auth_request;

/* Return granted/denied/pending for the current offer.  A missing callback is
 * treated as denied: the owner must explicitly opt in to projection.  The
 * callback may be queried again immediately before the offer is sent and on
 * each bounded tick, so it must be idempotent and cheap. */
typedef cp_boot_consent (*cp_boot_consent_fn)(void *context, uint64_t now_ms);

/* Return true while a USB (wired) projection session is active or starting
 * for this phone.  Identifiers may be empty when the phone has not told us
 * yet.  A missing callback is conservative: a wired offer blocks wireless
 * start, but a wireless-only offer is allowed.  Like consent, it may be
 * queried again at send time and on bounded ticks. */
typedef bool (*cp_boot_usb_active_fn)(void *context,
                                      const char *bluetooth_id,
                                      const char *usb_id);

typedef struct {
    const char *name;
    const char *model_identifier;
    const char *manufacturer;
    const char *serial_number;
    const char *firmware_version;
    const char *hardware_version;
    const char *eap_name;
    const char *current_language;
    const char *const *supported_languages;
    size_t supported_language_count;
    const char *app_match_team_id; /* optional */
    const char *bluetooth_transport_name;
    const char *bluetooth_mac; /* "AA:BB:CC:DD:EE:FF", real 6-byte MAC */
    uint16_t maximum_current_drawn_ma;

    const char *ap_ssid;
    const char *ap_passphrase;
    uint8_t ap_channel;
    cp_wire_wifi_security ap_security;
    const char *link_local; /* usable fe80::/10 address */
    uint32_t airplay_port;
    const char *device_identifier;
    const char *public_key;     /* optional */
    const char *source_version; /* optional */
    /* Bound in milliseconds for waiting on the AP readback and for the
     * post-start receiver offer.  0 selects CP_BOOT_DEFAULT_AP_WAIT_MS
     * (30000).  The same value is deliberately reused so existing callers
     * keep one bounded policy field; it is capped by CP_BOOT_MAX_AP_WAIT_MS. */
    uint32_t ap_wait_ms;
} cp_boot_config;

typedef struct {
    bool on_air;
    const char *ssid;
    uint8_t channel;
    cp_wire_wifi_security security;
    const char *link_local; /* optional override; must be usable */
} cp_boot_ap_readback;

typedef struct {
    cp_boot_event event;
    cp_boot_state state;
    cp_boot_error error;
    bool projection_active; /* always false in C1 */
    size_t out_len;
    uint64_t generation;
} cp_boot_result;

typedef struct {
    char name[CP_WIRE_MAX_IDENTITY + 1u];
    char model_identifier[CP_WIRE_MAX_IDENTITY + 1u];
    char manufacturer[CP_WIRE_MAX_IDENTITY + 1u];
    char serial_number[CP_WIRE_MAX_IDENTITY + 1u];
    char firmware_version[CP_WIRE_MAX_IDENTITY + 1u];
    char hardware_version[CP_WIRE_MAX_IDENTITY + 1u];
    char eap_name[CP_WIRE_MAX_EAP_NAME + 1u];
    char current_language[CP_WIRE_MAX_LANGUAGE + 1u];
    char supported_languages[CP_BOOT_MAX_LANGUAGES][CP_WIRE_MAX_LANGUAGE + 1u];
    size_t supported_language_count;
    char app_match_team_id[CP_WIRE_MAX_TEAM_ID + 1u];
    char bluetooth_transport_name[CP_WIRE_MAX_TRANSPORT_NAME + 1u];
    uint8_t bluetooth_mac[6];
    uint16_t maximum_current_drawn_ma;

    char ap_ssid[CP_WIRE_MAX_SSID + 1u];
    char ap_passphrase[CP_WIRE_MAX_PASSPHRASE + 1u];
    uint8_t ap_channel;
    cp_wire_wifi_security ap_security;
    char link_local[CP_WIRE_MAX_LINK_LOCAL + 1u];
    uint32_t airplay_port;
    char device_identifier[CP_WIRE_MAX_DEVICE_ID + 1u];
    char public_key[CP_WIRE_MAX_PUBLIC_KEY + 1u];
    char source_version[CP_WIRE_MAX_SOURCE_VERSION + 1u];
    uint32_t ap_wait_ms;

    cp_boot_auth_provider auth_provider;
    cp_boot_consent_fn consent;
    cp_boot_usb_active_fn usb_active;
    void *policy_context;

    uint64_t generation;
    uint64_t last_now_ms;
    cp_boot_state state;
    cp_boot_auth_state auth_state;

    bool identification_sent;
    bool identified;
    unsigned identification_sends;
    unsigned identification_retries;
    bool drop_app_match_team_id;

    char phone_bluetooth_id[CP_BOOT_MAX_TRANSPORT_ID + 1u];
    char phone_usb_id[CP_BOOT_MAX_TRANSPORT_ID + 1u];
    bool have_phone_bluetooth_id;
    bool have_phone_usb_id;
    bool wireless_update_known;
    bool wireless_update_available;

    bool offer_active;
    uint64_t offer_deadline_ms;
    bool offer_held;             /* trusted caller hold; never wire-derived */
    uint64_t offer_hold_generation;
    bool pending_start;
    uint64_t pending_deadline_ms;
    uint64_t pending_generation;

    cp_boot_ap_state ap_state;
    bool readback_valid;
    char readback_ssid[CP_WIRE_MAX_SSID + 1u];
    char readback_link_local[CP_WIRE_MAX_LINK_LOCAL + 1u];
    uint8_t readback_channel;
    cp_wire_wifi_security readback_security;

    uint8_t auth_scratch[CP_BOOT_MAX_AUTH_BLOB];
    unsigned auth_exchanges;
    bool async_auth;
    cp_boot_auth_request auth_request;
    uint64_t next_auth_token;
    /* Auth messages may arrive before 1D02; they are decoded and buffered, not
     * answered, until identification succeeds.  Credentials and 0x4301 stay
     * gated on AA05 after this buffer is serviced. */
    bool pending_auth_cert;
    bool have_pending_auth_challenge;
    size_t pending_auth_challenge_len;
    uint8_t pending_auth_challenge[CP_BOOT_MAX_AUTH_BLOB];
    bool projection_active; /* invariant: false */
} cp_bootstrap;

cp_boot_error cp_bootstrap_init(cp_bootstrap *session,
                                const cp_boot_config *config);
cp_boot_error cp_bootstrap_set_auth_provider(
    cp_bootstrap *session, const cp_boot_auth_provider *provider);
/* Select only before identification starts; synchronous providers are never
 * called in deferred mode. Stale request/completion identities are refused. */
cp_boot_error cp_bootstrap_set_async_auth(cp_bootstrap *session, bool enabled);
bool cp_bootstrap_auth_request(const cp_bootstrap *session,
                               cp_boot_auth_request *request);
cp_boot_result cp_bootstrap_auth_complete(
    cp_bootstrap *session, uint64_t generation, uint64_t token,
    cp_boot_auth_op op, bool success, const uint8_t *blob, size_t blob_len,
    uint64_t now_ms, uint8_t *out, size_t out_cap);
cp_boot_error cp_bootstrap_set_policy(cp_bootstrap *session,
                                      cp_boot_consent_fn consent,
                                      cp_boot_usb_active_fn usb_active,
                                      void *context);

/* Handle one complete phone-to-accessory frame.  The frame length may include
 * following coalesced frames; only the first CSM frame is consumed. */
cp_boot_result cp_bootstrap_receive(cp_bootstrap *session,
                                    const uint8_t *frame, size_t frame_len,
                                    uint64_t now_ms, uint8_t *out,
                                    size_t out_cap);

/* Advance pending AP/consent/USB deadlines and the bounded post-start offer.
 * Call periodically while a start is pending or an offer is outstanding; it
 * produces no traffic otherwise.  While an offer is outstanding it re-queries
 * consent and USB policy; denial, USB activity, wireless withdrawal, or expiry
 * invalidates the offer without sending a frame.  A held offer skips the
 * expiry branch but still runs the consent/USB/withdrawal checks. */
cp_boot_result cp_bootstrap_tick(cp_bootstrap *session, uint64_t now_ms,
                                 uint8_t *out, size_t out_cap);

/* Publish the AP's on-air readback.  A false/not-ready readback never makes
 * the AP ready; an invalid on-air readback is rejected without changing
 * state.  When a start is pending, a ready readback attempts it immediately. */
cp_boot_result cp_bootstrap_set_ap_readback(cp_bootstrap *session,
                                            const cp_boot_ap_readback *readback,
                                            uint64_t now_ms, uint8_t *out,
                                            size_t out_cap);

/* Explicit cancellation: clears a pending or unacknowledged start, any offer
 * hold, and bumps the generation so no stale deferred action can fire. */
cp_boot_result cp_bootstrap_cancel(cp_bootstrap *session, uint64_t now_ms);

/* Retire the post-start offer deadline while a trusted caller independently
 * observes an active receiver.  This is a trust boundary, not wire input:
 * callers MUST derive the observation from trusted local receiver/hardware
 * evidence bound to this boot generation and peer.  No network frame, CSM
 * field, scripted flag or unverified boolean may be forwarded here.
 *
 * The call succeeds only with `generation == cp_bootstrap_generation(session)`,
 * an existing offer, successful authentication and an identified peer.  It
 * does not verify hardware and cannot make projection_active true.  The hold
 * suppresses offer expiry/re-arm and duplicate starts only; availability is
 * still decoded, identity is still updated, and USB/consent policy is
 * re-queried before a held duplicate is returned.  Any invalidating event
 * clears the hold automatically (withdrawal, policy refusal, cancel, reset,
 * failure, generation change or AP-scope invalidation, including a valid but
 * changed on-air readback).  Clearing a hold never changes the offer, its
 * deadline or the generation. */
cp_boot_result cp_bootstrap_hold_offer(cp_bootstrap *session,
                                       uint64_t generation, uint64_t now_ms);

/* Drop a hold without changing the offer, generation or any queued reply.
 * The caller uses this when its own observation ends but the bounded offer
 * should continue to run normally. */
cp_boot_result cp_bootstrap_release_offer_hold(cp_bootstrap *session,
                                               uint64_t now_ms);

/* True only while a valid, generation-matching hold is in effect. */
bool cp_bootstrap_offer_held(const cp_bootstrap *session);

/* Transport loss: drops identification, authentication, phone identity and
 * any pending start.  AP readback is independent and is retained. */
cp_boot_result cp_bootstrap_transport_reset(cp_bootstrap *session,
                                           uint64_t now_ms);

cp_boot_state cp_bootstrap_state(const cp_bootstrap *session);
cp_boot_auth_state cp_bootstrap_auth_state(const cp_bootstrap *session);
cp_boot_ap_state cp_bootstrap_ap_state(const cp_bootstrap *session);
bool cp_bootstrap_projection_active(const cp_bootstrap *session);
uint64_t cp_bootstrap_generation(const cp_bootstrap *session);

#ifdef __cplusplus
}
#endif

#endif /* CP_BOOTSTRAP_H */
