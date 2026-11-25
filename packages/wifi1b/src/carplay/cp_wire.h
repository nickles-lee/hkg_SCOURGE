/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded, allocation-free codec for the iAP2 CarPlay control (CSM) messages
 * used by the first wireless-bootstrap checkpoint.
 *
 * Wire layout facts (CSM 0x4040 header, 16-bit big-endian total length and
 * message ID, TLV length including its own four-byte header) were verified
 * against the pinned LIVI reference tree
 * 64a05b68358db52ed887169143afd5944fd6f1f7.  No reference source is copied
 * or vendored here; this is an independent C11 implementation of the fields
 * that this checkpoint actually implements.
 *
 * This module never logs.  Callers must not log encoded frames either: the
 * WiFi configuration (0x5703) and start-session (0x4301) messages carry the
 * AP passphrase.  No function here allocates memory or reads configuration.
 */

#ifndef CP_WIRE_H
#define CP_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CP_WIRE_CSM_START 0x4040u
#define CP_WIRE_HEADER_LEN 6u
#define CP_WIRE_TLV_HEADER_LEN 4u

/* Bounded limits.  A frame larger than this is rejected instead of being
 * copied.  CP_WIRE_MAX_AUTH_BLOB is a local fixed-buffer policy, not a wire
 * fact: the actual OEM certificate and signature sizes are not known to this
 * tree, so callers must supply their real bytes within this bound.  The value
 * is a fixed buffer bound only, not a claim about any real certificate size. */
#define CP_WIRE_MAX_FRAME 4096u
#define CP_WIRE_MAX_TLV_DEPTH 8u
#define CP_WIRE_MAX_LANGUAGES 4u
#define CP_WIRE_MAX_AUTH_BLOB 2048u

#define CP_WIRE_MAX_IDENTITY 64u
#define CP_WIRE_MAX_LANGUAGE 16u
#define CP_WIRE_MAX_EAP_NAME 64u
#define CP_WIRE_MAX_TEAM_ID 64u
#define CP_WIRE_MAX_TRANSPORT_NAME 32u
#define CP_WIRE_MAX_SSID 32u
#define CP_WIRE_MAX_PASSPHRASE 63u
#define CP_WIRE_MAX_TRANSPORT_ID 128u
#define CP_WIRE_MAX_LINK_LOCAL 64u
#define CP_WIRE_MAX_DEVICE_ID 64u
#define CP_WIRE_MAX_PUBLIC_KEY 512u
#define CP_WIRE_MAX_SOURCE_VERSION 64u
#define CP_WIRE_MAX_CHALLENGE CP_WIRE_MAX_AUTH_BLOB

/* Message IDs implemented by this checkpoint. */
#define CP_WIRE_MSG_IDENTIFICATION_START 0x1D00u
#define CP_WIRE_MSG_IDENTIFICATION_INFO 0x1D01u
#define CP_WIRE_MSG_IDENTIFICATION_ACCEPTED 0x1D02u
#define CP_WIRE_MSG_IDENTIFICATION_REJECTED 0x1D03u
#define CP_WIRE_MSG_AUTH_CERT_REQUEST 0xAA00u
#define CP_WIRE_MSG_AUTH_CERTIFICATE 0xAA01u
#define CP_WIRE_MSG_AUTH_CHALLENGE_REQUEST 0xAA02u
#define CP_WIRE_MSG_AUTH_RESPONSE 0xAA03u
#define CP_WIRE_MSG_AUTH_FAILED 0xAA04u
#define CP_WIRE_MSG_AUTH_SUCCEEDED 0xAA05u
#define CP_WIRE_AUTH_CHALLENGE_SHA1 20u
#define CP_WIRE_AUTH_CHALLENGE_SHA256 32u
#define CP_WIRE_MSG_WIFI_CONFIG_REQUEST 0x5702u
#define CP_WIRE_MSG_WIFI_CONFIG_INFO 0x5703u
#define CP_WIRE_MSG_WIRELESS_UPDATE 0x4E0Du
#define CP_WIRE_MSG_TRANSPORT_IDS 0x4E0Eu
#define CP_WIRE_MSG_CARPLAY_AVAILABILITY 0x4300u
#define CP_WIRE_MSG_CARPLAY_START 0x4301u

typedef enum {
    CP_WIRE_OK = 0,
    CP_WIRE_END = 1,
    CP_WIRE_ERR_ARGUMENT = -1,
    CP_WIRE_ERR_BUFFER = -2,
    CP_WIRE_ERR_HEADER = -3,
    CP_WIRE_ERR_LENGTH = -4,
    CP_WIRE_ERR_TRUNCATED = -5,
    CP_WIRE_ERR_TLV = -6,
    CP_WIRE_ERR_MSG_ID = -7,
    CP_WIRE_ERR_DUPLICATE = -8,
    CP_WIRE_ERR_PARAM = -9,
    CP_WIRE_ERR_TYPE = -10,
    CP_WIRE_ERR_UTF8 = -11,
    CP_WIRE_ERR_MISSING = -12,
    CP_WIRE_ERR_UNSUPPORTED = -13,
    CP_WIRE_ERR_RANGE = -14,
    CP_WIRE_ERR_DIRECTION = -15
} cp_wire_status;

/* WiFi security vocabulary used by the start-session encoder.  These are
 * internal names; the wire values are mapped explicitly below. */
typedef enum {
    CP_WIRE_WIFI_WPA2 = 0,
    CP_WIRE_WIFI_WPA2_WPA3 = 1,
    CP_WIRE_WIFI_WPA3 = 2
} cp_wire_wifi_security;

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
    /* Optional; omitted when NULL or empty. */
    const char *app_match_team_id;
    const char *bluetooth_transport_name;
    uint8_t bluetooth_mac[6];
    const char *wireless_transport_name;
    uint16_t maximum_current_drawn_ma;
} cp_wire_identification;

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
} cp_wire_wifi_config;

typedef struct {
    const char *ssid;
    const char *passphrase;
    uint8_t channel;
    cp_wire_wifi_security security;
    const char *link_local;
    uint32_t port;
    const char *device_identifier;
    const char *public_key;
    const char *source_version;
} cp_wire_start_session;

typedef struct {
    /* Bits for recognized identification parameter IDs that the phone
     * rejected.  Flags for parameter IDs this checkpoint does not implement
     * are ignored. */
    uint32_t flagged;
} cp_wire_identification_rejection;

typedef struct {
    bool present;
    bool available_known;
    bool available;
    char transport_id[CP_WIRE_MAX_TRANSPORT_ID + 1u];
} cp_wire_availability_side;

typedef struct {
    cp_wire_availability_side wired;
    cp_wire_availability_side wireless;
} cp_wire_availability;

typedef struct {
    bool available;
} cp_wire_wireless_update;

typedef struct {
    char bluetooth_transport_id[CP_WIRE_MAX_TRANSPORT_ID + 1u];
    char usb_transport_id[CP_WIRE_MAX_TRANSPORT_ID + 1u];
} cp_wire_transport_ids;

typedef struct {
    const uint8_t *frame;
    size_t frame_len;
    uint16_t msg_id;
    const uint8_t *params;
    size_t params_len;
} cp_wire_view;

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t offset;
} cp_wire_tlv_iter;

/* Header and TLV utilities. */
cp_wire_status cp_wire_peek(const uint8_t *buf, size_t len, uint16_t *msg_id,
                            size_t *frame_len);
cp_wire_status cp_wire_open(const uint8_t *buf, size_t len, cp_wire_view *out);
cp_wire_status cp_wire_tlv_iter_init(cp_wire_tlv_iter *it, const uint8_t *params,
                                     size_t params_len);
cp_wire_status cp_wire_tlv_iter_next(cp_wire_tlv_iter *it, uint16_t *id,
                                     const uint8_t **payload, size_t *payload_len);

/* Text and identity validation helpers (no locale, no allocation).
 * cp_wire_link_local_usable accepts a bare fe80::/10 unicast address with a
 * nonzero interface identifier.  Zone suffixes are rejected because the wire
 * field carries the address only; the caller keeps the interface binding. */
bool cp_wire_utf8_valid(const uint8_t *data, size_t len);
bool cp_wire_link_local_usable(const char *text);
cp_wire_status cp_wire_parse_bt_mac(const char *text, uint8_t out[6]);
bool cp_wire_bt_mac_usable(const uint8_t mac[6]);

/* Encoders.  Every encoder writes one complete frame and reports the exact
 * number of bytes used.  A too-small output buffer is an error and the
 * output is left usable by the caller (the length is not reported). */
cp_wire_status cp_wire_encode_identification(const cp_wire_identification *id,
                                             uint8_t *out, size_t out_cap,
                                             size_t *out_len);
cp_wire_status cp_wire_encode_auth_certificate(const uint8_t *certificate,
                                               size_t certificate_len,
                                               uint8_t *out, size_t out_cap,
                                               size_t *out_len);
cp_wire_status cp_wire_encode_auth_response(const uint8_t *response,
                                            size_t response_len,
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len);
cp_wire_status cp_wire_encode_wifi_config(const cp_wire_wifi_config *cfg,
                                          uint8_t *out, size_t out_cap,
                                          size_t *out_len);
cp_wire_status cp_wire_encode_start_session(const cp_wire_start_session *start,
                                            uint8_t *out, size_t out_cap,
                                            size_t *out_len);

/* Decoders for the messages a phone sends during wireless bring-up.  They
 * structurally validate every TLV (length, bounded nesting, truncation),
 * reject duplicates of a known field and malformed nested TLVs, and never
 * return pointers past the frame. In messages with parameter tables, unknown
 * parameter IDs are skipped after
 * their length has been validated: the reference profile treats unknown
 * parameters as ignorable extensions, and this checkpoint must not turn an
 * unknown optional field into a fatal error.  String fields that this
 * checkpoint does implement keep strict NUL-termination and UTF-8 validity;
 * no form of unconditional trailing-NUL stripping is performed. Messages
 * defined as empty remain strictly empty; extension parameters are not
 * assumed valid for a message without a parameter table. */
cp_wire_status cp_wire_decode_empty(const cp_wire_view *view,
                                    uint16_t expected_msg_id);
cp_wire_status cp_wire_decode_identification_rejection(
    const cp_wire_view *view, cp_wire_identification_rejection *out);
cp_wire_status cp_wire_decode_challenge_request(const cp_wire_view *view,
                                                const uint8_t **challenge,
                                                size_t *challenge_len);
cp_wire_status cp_wire_decode_availability(const cp_wire_view *view,
                                           cp_wire_availability *out);
cp_wire_status cp_wire_decode_wireless_update(const cp_wire_view *view,
                                              cp_wire_wireless_update *out);
cp_wire_status cp_wire_decode_transport_ids(const cp_wire_view *view,
                                            cp_wire_transport_ids *out);

/* Optional diagnostics: a static string, never frame contents. */
const char *cp_wire_status_name(cp_wire_status status);

#ifdef __cplusplus
}
#endif

#endif /* CP_WIRE_H */
