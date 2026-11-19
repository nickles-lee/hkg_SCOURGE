/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded, one-shot lookup of the stock _airplay._tcp advertisement.
 *
 * The lookup is deliberately passive: it browses and resolves DNS-SD on one
 * interface, then stops.  It does not start, stop or configure the receiver,
 * and it does not claim that an mDNS record is locally attested.  Callers
 * must apply their own policy and local checks before using the result.
 *
 * The libdns_sd backend is loaded with dlopen only, so this module adds no
 * NEEDED entry.  Unless a caller explicitly enables the diagnostic override,
 * the pinned /app/lib/libdns_sd.so is hash-verified before it is loaded.
 *
 * Resolve references are never deallocated from inside a DNS-SD reply
 * callback.  A callback marks its fixed slot done; the ref is queued on a
 * fixed-size pending list and released after DNSServiceProcessResult returns.
 */

#ifndef CP_RECEIVER_LOOKUP_H
#define CP_RECEIVER_LOOKUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp_oem_receiver.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Distinct outcomes.  Callers must not collapse these into one state error. */
typedef enum {
    CP_RECEIVER_LOOKUP_OK = 0,
    CP_RECEIVER_LOOKUP_NO_SERVICE, /* browse ran, zero instances appeared */
    CP_RECEIVER_LOOKUP_TIMEOUT,    /* instances appeared, none resolved */
    CP_RECEIVER_LOOKUP_AMBIGUOUS,  /* more distinct instances than allowed */
    CP_RECEIVER_LOOKUP_DNS,        /* backend/process/transport failure */
    CP_RECEIVER_LOOKUP_STOPPED,    /* stop_fn asked us to stop */
    CP_RECEIVER_LOOKUP_ARGUMENT,   /* caller misuse */
    CP_RECEIVER_LOOKUP_NO_BACKEND, /* dlopen/dlsym failed */
    CP_RECEIVER_LOOKUP_BACKEND_PIN /* pinned libdns_sd missing/mismatched */
} cp_receiver_lookup_status;

/* State of one distinct browse instance after its resolve attempt. */
typedef enum {
    CP_RECEIVER_CANDIDATE_UNRESOLVED = 0,
    CP_RECEIVER_CANDIDATE_VALID,
    CP_RECEIVER_CANDIDATE_SRV, /* SRV/name/interface/port rejected */
    CP_RECEIVER_CANDIDATE_TXT, /* TXT missing, malformed or unparsable */
    CP_RECEIVER_CANDIDATE_RESOLVE_ERROR
} cp_receiver_candidate_state;

/*
 * One distinct (interface_index, service_name) instance.  The raw TXT pi
 * value is never copied here: only its length and SHA-256 leave the parser.
 */
typedef struct {
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    uint32_t interface_index;
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    size_t pi_len;
    bool have_pi;
    cp_receiver_candidate_state state;
} cp_receiver_observation;

typedef struct {
    /* Distinct instances observed, including ones that failed to resolve. */
    uint32_t service_count;
    /* Candidates that produced a valid SRV + TXT pair. */
    uint32_t resolved_count;
    /* Candidates whose resolve attempt was rejected. */
    uint32_t rejected_count;
    /* More distinct instances appeared than max_services allowed. */
    bool ambiguous;
    cp_receiver_observation services[CP_OEM_RECEIVER_MAX_SERVICES];
} cp_receiver_lookup_result;

typedef struct {
    char interface_name[CP_OEM_RECEIVER_MAX_INTERFACE];
    uint32_t interface_index;
    uint64_t timeout_ms;
    uint64_t settle_ms;
    uint32_t max_services;
    /*
     * Pin/load root.  "/" in the runtime path, so the verified file is
     * /app/lib/libdns_sd.so.  Offline fixtures may point it at a private
     * tree; the same root is used for verification and loading.
     */
    const char *backend_root;
    /*
     * Diagnostic-only override.  Honoured only when allow_unpinned_backend
     * is true.  The runtime path must leave this NULL.
     */
    const char *backend_library;
    bool allow_unpinned_backend;
} cp_receiver_lookup_config;

typedef int (*cp_receiver_stop_fn)(void *context);

/*
 * Resolve up to max_services distinct _airplay._tcp instances.
 *
 * Returns OK when every started resolve completed and no over-capacity
 * ambiguity was observed, even if every candidate was rejected; callers must
 * evaluate every candidate with policy so a specific SRV/TXT/PI refusal is
 * reported instead of a generic timeout (duplicates are deduplicated by
 * (interface_index, service_name)).
 * On every non-OK status the candidate array is zeroed; service_count,
 * resolved_count, rejected_count and ambiguous remain for diagnostics.
 *
 * stop_fn is polled at most every 200 ms so SIGTERM does not wait for the
 * full timeout.  Polling is bounded by timeout_ms and settle_ms; there is no
 * unbounded DNSServiceProcessResult loop.
 */
cp_receiver_lookup_status cp_receiver_lookup_live(
    const cp_receiver_lookup_config *config, cp_receiver_stop_fn stop_fn,
    void *stop_context, cp_receiver_lookup_result *out);

const char *cp_receiver_lookup_status_string(cp_receiver_lookup_status status);
const char *cp_receiver_candidate_state_string(
    cp_receiver_candidate_state state);

#ifdef __cplusplus
}
#endif

#endif /* CP_RECEIVER_LOOKUP_H */
