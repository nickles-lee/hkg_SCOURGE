/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Pure policy checks for the observed _airplay._tcp receiver.
 *
 * The mDNS observation is untrusted network input.  Policy evaluates every
 * resolved candidate, ties the advertised device ID to the locally configured
 * Bluetooth MAC (+1, the stock receiver's derivation), optionally pins the
 * advertised pairing identifier by SHA-256, and then requires a local LISTEN
 * socket owned by an allowlisted stock process.
 *
 * Never copy, log or compare the raw TXT pi value here.  Only pi_len and
 * pi_sha256 leave the lookup layer.
 */

#ifndef CP_RECEIVER_POLICY_H
#define CP_RECEIVER_POLICY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cp_receiver_lookup.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CP_RECEIVER_POLICY_OK = 0,
    CP_RECEIVER_POLICY_ERR_ARGUMENT,
    CP_RECEIVER_POLICY_ERR_NO_SERVICE,
    CP_RECEIVER_POLICY_ERR_AMBIGUOUS,
    CP_RECEIVER_POLICY_ERR_PORT,
    CP_RECEIVER_POLICY_ERR_DEVICE_ID,
    CP_RECEIVER_POLICY_ERR_SOURCE_VERSION,
    CP_RECEIVER_POLICY_ERR_TXT,
    CP_RECEIVER_POLICY_ERR_PI,
    CP_RECEIVER_POLICY_ERR_RESOLVE,
    CP_RECEIVER_POLICY_ERR_BT_MAC
} cp_receiver_policy_status;

typedef enum {
    CP_RECEIVER_LISTENER_OK = 0,
    CP_RECEIVER_LISTENER_ABSENT,
    CP_RECEIVER_LISTENER_SCOPE,
    CP_RECEIVER_LISTENER_OWNER,
    CP_RECEIVER_LISTENER_ARGUMENT,
    CP_RECEIVER_LISTENER_IO
} cp_receiver_listener_status;

typedef struct {
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    uint32_t interface_index;
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    size_t pi_len;
} cp_receiver_policy_result;

typedef struct {
    const char *bt_mac;
    const char *expected_device_id;
    unsigned expected_port;
    const char *expected_source_version;
    /* Optional SHA-256 pin (32 bytes); NULL means "no pin". */
    const uint8_t *expected_pi_sha256;
} cp_receiver_policy_params;

/*
 * Validate every candidate and produce the exact values to place in 0x4301.
 *
 * Exactly one passing identity is accepted.  Duplicate advertisements of the
 * same (device_id, pi, port) identity collapse to one; two passing candidates
 * with different identity are AMBIGUOUS.  Candidates rejected for SRV/TXT/
 * port/identity reasons do not abort the others; when none pass, the most
 * specific failure reason is reported.
 */
cp_receiver_policy_status cp_receiver_policy_accept(
    const cp_receiver_observation *observations, size_t count,
    const cp_receiver_policy_params *params,
    cp_receiver_policy_result *out);

/* Derive the stock receiver device ID: Bluetooth MAC with last byte +1.
 * The byte increment wraps at FF and is not a carry; that boundary is not
 * car-observed (the normal +1 relationship is). */
cp_receiver_policy_status cp_receiver_policy_derive_device_id(
    const char *bt_mac, char *out, size_t out_size);

/* Parse a 64-hex-character SHA-256 pin. */
bool cp_receiver_policy_pin_parse(const char *hex,
                                  uint8_t out[CP_OEM_RECEIVER_SHA256_LEN]);

/* JSON-escape an observed string.  Returns false (with a valid truncated
 * string) when the destination is too small. */
bool cp_receiver_json_escape(const char *in, char *out, size_t out_size);

typedef struct {
    const char *proc_root; /* normally "/proc"; fixtures may override */
    const char *interface_name;
    /*
     * Optional interface address list.  When NULL, the live interface is
     * queried with getifaddrs(); fixtures supply literals instead.
     */
    const char *const *interface_addresses;
    size_t interface_address_count;
    const char *owner_allowlist; /* comma separated; NULL => AppProjection */
    unsigned max_fd_scan;        /* 0 => a bounded default */
} cp_receiver_listener_config;

typedef struct {
    cp_receiver_listener_status status;
    bool found;
    bool wildcard;
    bool loopback_only;
    bool address_allowed;
    bool owner_allowed;
    unsigned long inode;
    int owner_pid;
    char owner_name[64];
} cp_receiver_listener_result;

/*
 * Check the local LISTEN socket for port.  Wildcard or receiver-interface
 * addresses are acceptable; loopback-only is LISTENER_SCOPE.  The socket
 * inode is mapped to a PID by a bounded /proc/<pid>/fd scan (readlink only,
 * no symlink following).  An unknown or non-allowlisted owner is
 * LISTENER_OWNER.  Findings are reported in out even when the status refuses,
 * so a diagnostic mode can print the owner without accepting it.
 */
cp_receiver_listener_status cp_receiver_listener_check(
    uint16_t port, const cp_receiver_listener_config *config,
    cp_receiver_listener_result *out);

/*
 * Listener checks require cp_receiver_listener_check(); there is no
 * port-presence-only helper because that would silently skip ownership.
 */
const char *cp_receiver_policy_status_string(cp_receiver_policy_status status);
const char *cp_receiver_listener_status_string(
    cp_receiver_listener_status status);

#ifdef __cplusplus
}
#endif

#endif /* CP_RECEIVER_POLICY_H */
