/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Passive, bounded observer for the OEM AirPlay receiver advertisement.
 *
 * This module parses only the DNS-SD SRV/TXT record for _airplay._tcp on the
 * target interface.  mDNS is network input and is untrusted: a parsed record
 * is reported as OBSERVED_UNVERIFIED unless the caller supplies a trusted
 * local attestation (process/listener/identity fingerprint) that matches it.
 *
 * The module does not start, stop, configure or query the OEM receiver.  It
 * does not load OEM libraries, open sockets, scan processes or observe
 * projection.  CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE is deliberate until a
 * separate runtime observer is implemented and validated.
 *
 * A successful observation is not a readiness decision.  The caller owns
 * policy.  The snapshot keeps observation, evidence, readiness and projection
 * separate so a broker cannot accidentally turn a peer advertisement into a
 * trusted state.
 *
 * The public_key field of the wireless start message is not produced here.
 * Its type and encoding belong to protocol work; this module does not assume
 * an Ed25519 or raw/hex representation and does not read keychain material.
 * For the 0x4301 start-session fields, snapshot.port, snapshot.device_id and
 * snapshot.source_version are the receiver-advertised values.  snapshot.pi is
 * the pairing identifier.  This module does not synthesize a device_id from a
 * Bluetooth MAC and does not populate public_key.
 *
 * The probe is single-owner: one thread calls the API and the DNS-SD backend
 * fires events on that same thread.  There are no allocations, threads or
 * internal clocks.  Callers supply monotonic milliseconds.
 */

#ifndef CP_OEM_RECEIVER_H
#define CP_OEM_RECEIVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CP_OEM_RECEIVER_SERVICE_TYPE "_airplay._tcp"
#define CP_OEM_RECEIVER_SERVICE_DOMAIN "local."
#define CP_OEM_RECEIVER_DEFAULT_INTERFACE "wlan0"
#define CP_OEM_RECEIVER_SOURCE_VERSION "320.17.6"

#define CP_OEM_RECEIVER_MAX_SERVICE_NAME 128u
#define CP_OEM_RECEIVER_MAX_HOSTTARGET 256u
#define CP_OEM_RECEIVER_MAX_TXT 1024u
#define CP_OEM_RECEIVER_MAX_DEVICE_ID 64u
#define CP_OEM_RECEIVER_MAX_PI 128u
#define CP_OEM_RECEIVER_MAX_SRCVERS 32u
#define CP_OEM_RECEIVER_MAX_INTERFACE 16u
#define CP_OEM_RECEIVER_MAX_LINK_LOCAL 64u
#define CP_OEM_RECEIVER_MAX_SERVICES 4u
#define CP_OEM_RECEIVER_MAX_TXT_KEYS 32u
#define CP_OEM_RECEIVER_SHA256_LEN 32u

typedef enum {
    CP_OEM_RECEIVER_OK = 0,
    CP_OEM_RECEIVER_ERR_ARGUMENT,
    CP_OEM_RECEIVER_ERR_BOUNDS,
    CP_OEM_RECEIVER_ERR_STATE,
    CP_OEM_RECEIVER_ERR_NO_BACKEND,
    CP_OEM_RECEIVER_ERR_DNS,
    CP_OEM_RECEIVER_ERR_TXT_MALFORMED,
    CP_OEM_RECEIVER_ERR_TXT_DUPLICATE,
    CP_OEM_RECEIVER_ERR_TXT_MISSING,
    CP_OEM_RECEIVER_ERR_TXT_UTF8,
    CP_OEM_RECEIVER_ERR_TXT_VALUE,
    CP_OEM_RECEIVER_ERR_SRV_PORT,
    CP_OEM_RECEIVER_ERR_SRV_INTERFACE,
    CP_OEM_RECEIVER_ERR_SRV_NAME,
    CP_OEM_RECEIVER_ERR_SCOPE,
    CP_OEM_RECEIVER_ERR_IO,
    CP_OEM_RECEIVER_ERR_HASH,
    CP_OEM_RECEIVER_ERR_PIN_MISMATCH
} cp_oem_receiver_status;

typedef enum {
    CP_OEM_RECEIVER_STATE_IDLE = 0,
    CP_OEM_RECEIVER_STATE_WAITING,
    CP_OEM_RECEIVER_STATE_OBSERVED,
    CP_OEM_RECEIVER_STATE_REMOVED,
    CP_OEM_RECEIVER_STATE_TIMEOUT,
    CP_OEM_RECEIVER_STATE_ERROR
} cp_oem_receiver_state;

typedef enum {
    CP_OEM_RECEIVER_EVIDENCE_NONE = 0,
    CP_OEM_RECEIVER_EVIDENCE_MDNS_UNVERIFIED,
    CP_OEM_RECEIVER_EVIDENCE_LOCAL_ATTESTED
} cp_oem_receiver_evidence;

typedef enum {
    CP_OEM_RECEIVER_READINESS_NO_OBSERVATION = 0,
    CP_OEM_RECEIVER_READINESS_INTERFACE_NOT_READY,
    CP_OEM_RECEIVER_READINESS_OBSERVED_UNVERIFIED,
    CP_OEM_RECEIVER_READINESS_LOCAL_MATCHED,
    CP_OEM_RECEIVER_READINESS_AMBIGUOUS,
    CP_OEM_RECEIVER_READINESS_STALE,
    CP_OEM_RECEIVER_READINESS_TIMEOUT
} cp_oem_receiver_readiness;

typedef enum {
    /* No projection observer exists in this module.  Do not add an "active"
     * value here without a separately validated runtime observer. */
    CP_OEM_RECEIVER_PROJECTION_UNAVAILABLE = 0
} cp_oem_receiver_projection;

typedef enum {
    CP_OEM_RECEIVER_BLOCKER_NONE = 0u,
    CP_OEM_RECEIVER_BLOCKER_NO_OBSERVATION = 1u << 0,
    CP_OEM_RECEIVER_BLOCKER_INTERFACE_NOT_READY = 1u << 1,
    CP_OEM_RECEIVER_BLOCKER_LINK_LOCAL_UNUSABLE = 1u << 2,
    CP_OEM_RECEIVER_BLOCKER_LOCAL_ADDRESS_UNCONFIRMED = 1u << 3,
    CP_OEM_RECEIVER_BLOCKER_LOCAL_RECEIVER_NOT_OBSERVED = 1u << 4,
    CP_OEM_RECEIVER_BLOCKER_LOCAL_PORT_MISMATCH = 1u << 5,
    CP_OEM_RECEIVER_BLOCKER_IDENTITY_ATTESTATION_MISSING = 1u << 6,
    CP_OEM_RECEIVER_BLOCKER_LOCAL_IDENTITY_MISMATCH = 1u << 7,
    CP_OEM_RECEIVER_BLOCKER_AMBIGUOUS_SERVICES = 1u << 8,
    CP_OEM_RECEIVER_BLOCKER_TOO_MANY_SERVICES = 1u << 9,
    CP_OEM_RECEIVER_BLOCKER_DNS_ERROR = 1u << 10,
    CP_OEM_RECEIVER_BLOCKER_TIMEOUT = 1u << 11,
    CP_OEM_RECEIVER_BLOCKER_STALE = 1u << 12,
    CP_OEM_RECEIVER_BLOCKER_NOT_SETTLED = 1u << 13,
    CP_OEM_RECEIVER_BLOCKER_UNEXPECTED_SOURCE_VERSION = 1u << 14,
    CP_OEM_RECEIVER_BLOCKER_IDENTITY_ADAPTER_UNIMPLEMENTED = 1u << 15,
    CP_OEM_RECEIVER_BLOCKER_TXT_INVALID = 1u << 16,
    CP_OEM_RECEIVER_BLOCKER_SRV_INVALID = 1u << 17,
    CP_OEM_RECEIVER_BLOCKER_RESOLVE_TIMEOUT = 1u << 18
} cp_oem_receiver_blocker;

#define CP_OEM_RECEIVER_CHANGED_OBSERVATION (1u << 0)
#define CP_OEM_RECEIVER_CHANGED_READINESS (1u << 1)
#define CP_OEM_RECEIVER_CHANGED_SERVICES (1u << 2)
#define CP_OEM_RECEIVER_CHANGED_ERROR (1u << 3)

typedef struct {
    char interface_name[CP_OEM_RECEIVER_MAX_INTERFACE];
    uint32_t interface_index; /* 0 until the caller supplies a verified index */
    uint64_t browse_timeout_ms;
    uint64_t resolve_timeout_ms;
    uint64_t settle_ms;
    uint64_t stale_after_ms;
    uint32_t max_services;
} cp_oem_receiver_config;

typedef struct {
    char interface_name[CP_OEM_RECEIVER_MAX_INTERFACE];
    uint32_t interface_index;
    char link_local[CP_OEM_RECEIVER_MAX_LINK_LOCAL]; /* bare address, no zone */
    bool has_link_local;
    bool zone_present;
    bool zone_matches;
    bool local_address_confirmed;
} cp_oem_receiver_scope;

typedef struct {
    bool receiver_observed;
    uint32_t receiver_pid;
    uint16_t listener_port;
    bool identity_fingerprint_present;
    uint8_t identity_fingerprint[CP_OEM_RECEIVER_SHA256_LEN];
} cp_oem_receiver_attestation;

/* A native AirPlay keychain identity adapter is deliberately not implemented
 * in this first step.  The probe accepts only a caller-supplied fingerprint
 * from a future trusted broker.  It never creates, copies, deletes or dumps
 * keychain identity material and does not assume a key type or encoding.
 * The receiver's advertised device_id is reported as observed; it is not
 * synthesized from a Bluetooth MAC. */

typedef struct {
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    size_t device_id_len;
    char pi[CP_OEM_RECEIVER_MAX_PI];
    size_t pi_len;
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    size_t source_version_len;
    bool have_device_id;
    bool have_pi;
    bool have_source_version;
    uint16_t key_count;
    uint16_t unknown_count;
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
} cp_oem_receiver_txt;

typedef enum {
    CP_OEM_RECEIVER_DNS_ADD = 0,
    CP_OEM_RECEIVER_DNS_REMOVE,
    CP_OEM_RECEIVER_DNS_RESOLVE,
    CP_OEM_RECEIVER_DNS_ERROR
} cp_oem_receiver_dns_event_type;

typedef struct {
    cp_oem_receiver_dns_event_type type;
    uint64_t observed_ms; /* 0 means "use the probe's last caller time" */
    uint32_t interface_index;
    const char *service_name;
    size_t service_name_len;
    const char *regtype;
    size_t regtype_len;
    const char *domain;
    size_t domain_len;
    const char *hosttarget;
    size_t hosttarget_len;
    uint16_t port; /* host byte order */
    const uint8_t *txt;
    size_t txt_len;
    int32_t error;
} cp_oem_receiver_dns_event;

typedef void (*cp_oem_receiver_dns_event_fn)(void *context,
                                             const cp_oem_receiver_dns_event *event);

/*
 * A real backend owns the DNS-SD browse and resolve references.  It starts a
 * resolve for each browse ADD and emits ADD, RESOLVE and REMOVE events.  The
 * core validates every event and never assumes the backend is trustworthy.
 * start/stop/rebrowse must not block; event_fn is called on the caller thread.
 */
typedef struct {
    void *context;
    cp_oem_receiver_status (*start)(void *context, const char *service_type,
                                    const char *domain, uint32_t interface_index,
                                    cp_oem_receiver_dns_event_fn event_fn,
                                    void *event_context);
    void (*stop)(void *context);
    /* Optional.  If NULL the core calls stop() then start() on interface change. */
    cp_oem_receiver_status (*rebrowse)(void *context, uint32_t interface_index);
} cp_oem_receiver_dns_ops;

typedef struct {
    bool used;
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    size_t service_name_len;
    uint32_t interface_index;
    uint64_t added_ms;
    uint64_t resolve_deadline_ms;
    bool resolved;
    bool compatible;
    bool stale;
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    size_t device_id_len;
    char pi[CP_OEM_RECEIVER_MAX_PI];
    size_t pi_len;
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    size_t source_version_len;
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    uint64_t resolved_ms;
    uint64_t last_update_ms;
} cp_oem_receiver_service;

typedef struct {
    cp_oem_receiver_state state;
    cp_oem_receiver_evidence evidence;
    cp_oem_receiver_readiness readiness;
    cp_oem_receiver_projection projection;
    uint32_t blockers;
    uint32_t observation_generation;
    uint32_t readiness_generation;
    bool stable;
    bool ambiguous;
    bool local_trusted;
    uint32_t service_count;
    uint32_t interface_index;
    char interface_name[CP_OEM_RECEIVER_MAX_INTERFACE];
    char link_local[CP_OEM_RECEIVER_MAX_LINK_LOCAL];
    bool link_local_usable;
    bool local_address_confirmed;
    bool interface_ready;
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    size_t device_id_len;
    /* pi is the advertised pairing identifier.  It is returned for internal
     * matching only; do not log it.  Use safe_summary for diagnostics. */
    char pi[CP_OEM_RECEIVER_MAX_PI];
    size_t pi_len;
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    size_t source_version_len;
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    bool local_port_matches;
    bool local_identity_matches;
} cp_oem_receiver_snapshot;

typedef struct {
    cp_oem_receiver_config config;
    cp_oem_receiver_dns_ops ops;
    bool started;
    uint64_t now_ms;
    uint64_t start_ms;
    uint64_t browse_deadline_ms;
    uint64_t settle_deadline_ms;
    uint32_t observation_generation;
    uint32_t readiness_generation;
    cp_oem_receiver_scope scope;
    cp_oem_receiver_attestation attestation;
    cp_oem_receiver_service services[CP_OEM_RECEIVER_MAX_SERVICES];
    uint32_t service_count;
    int selected;
    bool selected_valid;
    bool stable;
    bool ambiguous;
    bool dns_error;
    bool stale_reported;
    bool timeout_reported;
    uint32_t event_blockers;
    cp_oem_receiver_state state;
    cp_oem_receiver_evidence evidence;
    cp_oem_receiver_readiness readiness;
    uint32_t blockers;
    uint64_t first_seen_ms;
    uint64_t last_seen_ms;
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    size_t device_id_len;
    char pi[CP_OEM_RECEIVER_MAX_PI];
    size_t pi_len;
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    size_t source_version_len;
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    bool local_port_matches;
    bool local_identity_matches;
    bool reported_local_trusted;
    cp_oem_receiver_readiness reported_readiness;
    cp_oem_receiver_evidence reported_evidence;
    uint32_t reported_blockers;
    uint32_t reported_observation_generation;
    uint32_t reported_readiness_generation;
} cp_oem_receiver;

typedef struct {
    const char *relative_path;
    const char *sha256_hex;
} cp_oem_receiver_file_pin;

/* Target pins are local to this module until the shared firmware lock table
 * is ready.  These are identifiers only, not OEM content. */
extern const cp_oem_receiver_file_pin cp_oem_receiver_target_pins[];
extern const size_t cp_oem_receiver_target_pin_count;
extern const cp_oem_receiver_file_pin cp_oem_receiver_dns_sd_pin;

void cp_oem_receiver_config_default(cp_oem_receiver_config *out);

cp_oem_receiver_status cp_oem_receiver_init(cp_oem_receiver *probe,
                                            const cp_oem_receiver_config *config,
                                            const cp_oem_receiver_dns_ops *ops);
cp_oem_receiver_status cp_oem_receiver_start(cp_oem_receiver *probe, uint64_t now_ms);
void cp_oem_receiver_stop(cp_oem_receiver *probe);

cp_oem_receiver_status cp_oem_receiver_set_scope(cp_oem_receiver *probe,
                                                 const cp_oem_receiver_scope *scope,
                                                 uint64_t now_ms);
cp_oem_receiver_status cp_oem_receiver_set_attestation(
    cp_oem_receiver *probe, const cp_oem_receiver_attestation *attestation, uint64_t now_ms);

cp_oem_receiver_status cp_oem_receiver_on_dns_event(
    cp_oem_receiver *probe, const cp_oem_receiver_dns_event *event, uint64_t now_ms);
cp_oem_receiver_status cp_oem_receiver_poll(cp_oem_receiver *probe, uint64_t now_ms,
                                            uint32_t *changed);

cp_oem_receiver_status cp_oem_receiver_get_snapshot(const cp_oem_receiver *probe,
                                                    cp_oem_receiver_snapshot *out);
cp_oem_receiver_status cp_oem_receiver_safe_summary(const cp_oem_receiver_snapshot *snapshot,
                                                    char *buf, size_t buf_len);

/* Pure parsers and validators.  parse_txt requires the three advertised keys
 * deviceid, pi and srcvers, validates MAC/UTF-8/NUL/duplicate bounds, and
 * computes a SHA-256 fingerprint of the exact pi bytes.  Source-version
 * compatibility is a probe decision, not a parser decision. */
cp_oem_receiver_status cp_oem_receiver_parse_txt(const uint8_t *txt, size_t txt_len,
                                                 cp_oem_receiver_txt *out);
cp_oem_receiver_status cp_oem_receiver_validate_srv(uint32_t interface_index,
                                                     uint32_t target_interface_index,
                                                     uint16_t port,
                                                     const char *service_name,
                                                     size_t service_name_len);
bool cp_oem_receiver_link_local_usable(const char *text);
cp_oem_receiver_status cp_oem_receiver_scope_init(cp_oem_receiver_scope *out,
                                                   const char *interface_name,
                                                   uint32_t interface_index,
                                                   const char *link_local,
                                                   bool local_address_confirmed);
bool cp_oem_receiver_scope_ready(const cp_oem_receiver_scope *scope);
cp_oem_receiver_status cp_oem_receiver_scope_format(const cp_oem_receiver_scope *scope,
                                                    bool include_zone, char *out,
                                                    size_t out_len);

/* SHA-256 and local pin verification. File hashing rejects a leaf symlink;
 * the caller must supply an owner/root-controlled root and ancestor paths. */
cp_oem_receiver_status cp_oem_receiver_sha256(const void *data, size_t len,
                                              uint8_t out[CP_OEM_RECEIVER_SHA256_LEN]);
cp_oem_receiver_status cp_oem_receiver_sha256_file(
    const char *path, uint8_t out[CP_OEM_RECEIVER_SHA256_LEN]);
cp_oem_receiver_status cp_oem_receiver_verify_files(const char *root,
                                                    const cp_oem_receiver_file_pin *pins,
                                                    size_t pin_count, size_t *failed_index);
/* failed_index is 0..4 for the five target pins and 5 for libdns_sd. */
cp_oem_receiver_status cp_oem_receiver_verify_target_root(const char *root,
                                                          size_t *failed_index);

const char *cp_oem_receiver_status_string(cp_oem_receiver_status status);
const char *cp_oem_receiver_state_string(cp_oem_receiver_state state);
const char *cp_oem_receiver_evidence_string(cp_oem_receiver_evidence evidence);
const char *cp_oem_receiver_readiness_string(cp_oem_receiver_readiness readiness);
const char *cp_oem_receiver_projection_string(cp_oem_receiver_projection projection);

#ifdef __cplusplus
}
#endif

#endif /* CP_OEM_RECEIVER_H */
