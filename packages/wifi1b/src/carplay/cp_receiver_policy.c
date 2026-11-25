/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1

#include "cp_receiver_policy.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cp_wire.h"

#define CP_RECEIVER_LISTENER_DEFAULT_FDS 4096u
#define CP_RECEIVER_LISTENER_MAX_PIDS 8192u
#define CP_RECEIVER_LISTENER_MAX_INODES 8u
#define CP_RECEIVER_LISTENER_STATE_LISTEN 0x0Au

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool normalize_mac(const char *text, char out[18])
{
    size_t i;

    if (text == NULL || strlen(text) != 17u) {
        return false;
    }
    for (i = 0u; i < 17u; i++) {
        if (i == 2u || i == 5u || i == 8u || i == 11u || i == 14u) {
            if (text[i] != ':') {
                return false;
            }
            out[i] = ':';
            continue;
        }
        int value = hex_value(text[i]);
        if (value < 0) {
            return false;
        }
        out[i] = (char)(value < 10 ? '0' + value : 'A' + (value - 10));
    }
    out[17] = '\0';
    return true;
}

static bool normalize_usable_mac(const char *text, char out[18])
{
    uint8_t bytes[6];

    if (!normalize_mac(text, out)) {
        return false;
    }
    /* normalize_mac checks syntax only.  A pinned/observed identity must
     * also be a usable unicast, non-zero address; cp_wire already owns that
     * rule and the derived BT+1 path uses it too. */
    return cp_wire_parse_bt_mac(out, bytes) == CP_WIRE_OK;
}

cp_receiver_policy_status cp_receiver_policy_derive_device_id(
    const char *bt_mac, char *out, size_t out_size)
{
    uint8_t mac[6];
    int written;

    if (bt_mac == NULL || out == NULL || out_size == 0u) {
        return CP_RECEIVER_POLICY_ERR_ARGUMENT;
    }
    if (cp_wire_parse_bt_mac(bt_mac, mac) != CP_WIRE_OK) {
        return CP_RECEIVER_POLICY_ERR_BT_MAC;
    }
    /* Match the normal stock last-byte +1 derivation with a bare uint8
     * increment.  At ..:FF this wraps to ..:00 and does not carry into the
     * previous byte; that boundary is not car-observed, and the test records
     * the chosen behavior so a future change cannot happen silently. */
    mac[5]++;
    written = snprintf(out, out_size, "%02X:%02X:%02X:%02X:%02X:%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (written < 0 || (size_t)written >= out_size) {
        return CP_RECEIVER_POLICY_ERR_ARGUMENT;
    }
    return CP_RECEIVER_POLICY_OK;
}

bool cp_receiver_policy_pin_parse(const char *hex,
                                  uint8_t out[CP_OEM_RECEIVER_SHA256_LEN])
{
    size_t i;

    if (hex == NULL || out == NULL || strlen(hex) != CP_OEM_RECEIVER_SHA256_LEN * 2u) {
        return false;
    }
    for (i = 0u; i < CP_OEM_RECEIVER_SHA256_LEN; i++) {
        int high = hex_value(hex[i * 2u]);
        int low = hex_value(hex[i * 2u + 1u]);

        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = (uint8_t)((high << 4) | low);
    }
    return true;
}

bool cp_receiver_json_escape(const char *in, char *out, size_t out_size)
{
    size_t used = 0u;
    size_t i;
    bool truncated = false;

    if (out == NULL || out_size == 0u) {
        return false;
    }
    if (in == NULL) {
        in = "";
    }
    for (i = 0u; in[i] != '\0'; i++) {
        unsigned char c = (unsigned char)in[i];
        char encoded[7];
        size_t encoded_len;

        switch (c) {
        case '"':
            memcpy(encoded, "\\\"", 3u);
            encoded_len = 2u;
            break;
        case '\\':
            memcpy(encoded, "\\\\", 3u);
            encoded_len = 2u;
            break;
        case '\b':
            memcpy(encoded, "\\b", 3u);
            encoded_len = 2u;
            break;
        case '\f':
            memcpy(encoded, "\\f", 3u);
            encoded_len = 2u;
            break;
        case '\n':
            memcpy(encoded, "\\n", 3u);
            encoded_len = 2u;
            break;
        case '\r':
            memcpy(encoded, "\\r", 3u);
            encoded_len = 2u;
            break;
        case '\t':
            memcpy(encoded, "\\t", 3u);
            encoded_len = 2u;
            break;
        default:
            if (c < 0x20u) {
                static const char hexdigits[] = "0123456789abcdef";
                encoded[0] = '\\';
                encoded[1] = 'u';
                encoded[2] = '0';
                encoded[3] = '0';
                encoded[4] = hexdigits[(c >> 4) & 0x0fu];
                encoded[5] = hexdigits[c & 0x0fu];
                encoded_len = 6u;
            } else {
                encoded[0] = (char)c;
                encoded_len = 1u;
            }
            break;
        }
        if (used + encoded_len + 1u > out_size) {
            truncated = true;
            break;
        }
        memcpy(out + used, encoded, encoded_len);
        used += encoded_len;
    }
    out[used] = '\0';
    return !truncated;
}

static unsigned policy_rank(cp_receiver_policy_status status)
{
    switch (status) {
    case CP_RECEIVER_POLICY_ERR_PORT:
        return 1u;
    case CP_RECEIVER_POLICY_ERR_DEVICE_ID:
        return 2u;
    case CP_RECEIVER_POLICY_ERR_SOURCE_VERSION:
        return 3u;
    case CP_RECEIVER_POLICY_ERR_TXT:
        return 4u;
    case CP_RECEIVER_POLICY_ERR_RESOLVE:
        return 4u;
    case CP_RECEIVER_POLICY_ERR_PI:
        return 5u;
    default:
        return 0u;
    }
}

static bool copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t len;

    if (dst == NULL || cap == 0u || src == NULL) {
        return false;
    }
    len = strnlen(src, cap);
    if (len >= cap) {
        return false;
    }
    memcpy(dst, src, len + 1u);
    return true;
}

static cp_receiver_policy_status validate_candidate(
    const cp_receiver_observation *observation, const char *expected_mac,
    const cp_receiver_policy_params *params,
    cp_receiver_policy_result *candidate)
{
    char observed_mac[18];
    char expected_device_id[18];

    if (observation->state == CP_RECEIVER_CANDIDATE_SRV) {
        return CP_RECEIVER_POLICY_ERR_PORT;
    }
    if (observation->state == CP_RECEIVER_CANDIDATE_TXT) {
        return CP_RECEIVER_POLICY_ERR_TXT;
    }
    if (observation->state != CP_RECEIVER_CANDIDATE_VALID) {
        return CP_RECEIVER_POLICY_ERR_RESOLVE;
    }
    if (observation->port == 0u) {
        return CP_RECEIVER_POLICY_ERR_PORT;
    }
    if (observation->device_id[0] == '\0' ||
        !normalize_usable_mac(observation->device_id, observed_mac)) {
        return CP_RECEIVER_POLICY_ERR_DEVICE_ID;
    }
    if (params->expected_device_id != NULL &&
        params->expected_device_id[0] != '\0') {
        if (!normalize_usable_mac(params->expected_device_id,
                                  expected_device_id) ||
            strcmp(observed_mac, expected_device_id) != 0) {
            return CP_RECEIVER_POLICY_ERR_DEVICE_ID;
        }
    } else if (strcmp(observed_mac, expected_mac) != 0) {
        return CP_RECEIVER_POLICY_ERR_DEVICE_ID;
    }
    if (params->expected_port != 0u &&
        observation->port != (uint16_t)params->expected_port) {
        return CP_RECEIVER_POLICY_ERR_PORT;
    }
    if (observation->source_version[0] == '\0') {
        return CP_RECEIVER_POLICY_ERR_SOURCE_VERSION;
    }
    if (params->expected_source_version != NULL &&
        params->expected_source_version[0] != '\0' &&
        strcmp(observation->source_version,
               params->expected_source_version) != 0) {
        return CP_RECEIVER_POLICY_ERR_SOURCE_VERSION;
    }
    if (!observation->have_pi || observation->pi_len == 0u) {
        return CP_RECEIVER_POLICY_ERR_PI;
    }
    if (params->expected_pi_sha256 != NULL &&
        memcmp(params->expected_pi_sha256, observation->pi_sha256,
               CP_OEM_RECEIVER_SHA256_LEN) != 0) {
        return CP_RECEIVER_POLICY_ERR_PI;
    }
    memset(candidate, 0, sizeof(*candidate));
    candidate->port = observation->port;
    candidate->interface_index = observation->interface_index;
    candidate->pi_len = observation->pi_len;
    memcpy(candidate->pi_sha256, observation->pi_sha256,
           sizeof(candidate->pi_sha256));
    if (!copy_bounded(candidate->device_id, sizeof(candidate->device_id),
                      observation->device_id) ||
        !copy_bounded(candidate->source_version,
                      sizeof(candidate->source_version),
                      observation->source_version) ||
        !copy_bounded(candidate->service_name,
                      sizeof(candidate->service_name),
                      observation->service_name)) {
        return CP_RECEIVER_POLICY_ERR_ARGUMENT;
    }
    return CP_RECEIVER_POLICY_OK;
}

static bool same_identity(const cp_receiver_policy_result *a,
                          const cp_receiver_policy_result *b)
{
    return a->port == b->port && a->pi_len == b->pi_len &&
           memcmp(a->pi_sha256, b->pi_sha256, sizeof(a->pi_sha256)) == 0 &&
           strcmp(a->device_id, b->device_id) == 0;
}

cp_receiver_policy_status cp_receiver_policy_accept(
    const cp_receiver_observation *observations, size_t count,
    const cp_receiver_policy_params *params,
    cp_receiver_policy_result *out)
{
    cp_receiver_policy_result accepted[CP_OEM_RECEIVER_MAX_SERVICES];
    size_t accepted_count = 0u;
    cp_receiver_policy_status best = CP_RECEIVER_POLICY_ERR_NO_SERVICE;
    unsigned best_rank = 0u;
    char expected_mac[18];
    cp_receiver_policy_status status;
    size_t i;

    if (observations == NULL || params == NULL || out == NULL ||
        count > CP_OEM_RECEIVER_MAX_SERVICES) {
        return CP_RECEIVER_POLICY_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (count == 0u) {
        return CP_RECEIVER_POLICY_ERR_NO_SERVICE;
    }
    if (params->bt_mac == NULL || params->expected_port > 65535u) {
        return CP_RECEIVER_POLICY_ERR_ARGUMENT;
    }
    if (params->expected_device_id != NULL &&
        params->expected_device_id[0] != '\0') {
        if (!normalize_usable_mac(params->expected_device_id, expected_mac)) {
            return CP_RECEIVER_POLICY_ERR_DEVICE_ID;
        }
    } else {
        status = cp_receiver_policy_derive_device_id(
            params->bt_mac, expected_mac, sizeof(expected_mac));
        if (status != CP_RECEIVER_POLICY_OK) {
            return status;
        }
    }
    for (i = 0u; i < count; i++) {
        cp_receiver_policy_result candidate;

        status = validate_candidate(&observations[i], expected_mac, params,
                                    &candidate);
        if (status == CP_RECEIVER_POLICY_OK) {
            accepted[accepted_count++] = candidate;
        } else if (policy_rank(status) > best_rank) {
            best = status;
            best_rank = policy_rank(status);
        }
    }
    if (accepted_count == 0u) {
        return best;
    }
    for (i = 1u; i < accepted_count; i++) {
        if (!same_identity(&accepted[0], &accepted[i])) {
            return CP_RECEIVER_POLICY_ERR_AMBIGUOUS;
        }
    }
    *out = accepted[0];
    return CP_RECEIVER_POLICY_OK;
}

/* ------------------------------------------------------------------------- */
/* Listener scope and owner                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t bytes[16];
    size_t len; /* 4 or 16 */
} cp_addr;

static bool parse_hex_word(const char *text, unsigned long *out)
{
    unsigned long value = 0u;
    size_t i;

    for (i = 0u; i < 8u; i++) {
        int digit = hex_value(text[i]);

        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (unsigned long)digit;
    }
    *out = value;
    return true;
}

static bool parse_proc_addr(const char *text, cp_addr *out)
{
    size_t len;

    if (text == NULL || out == NULL) {
        return false;
    }
    len = strlen(text);
    memset(out, 0, sizeof(*out));
    if (len == 8u) {
        unsigned long word;

        if (!parse_hex_word(text, &word)) {
            return false;
        }
        out->len = 4u;
        out->bytes[0] = (uint8_t)(word & 0xffu);
        out->bytes[1] = (uint8_t)((word >> 8) & 0xffu);
        out->bytes[2] = (uint8_t)((word >> 16) & 0xffu);
        out->bytes[3] = (uint8_t)((word >> 24) & 0xffu);
        return true;
    }
    if (len == 32u) {
        size_t word;

        out->len = 16u;
        for (word = 0u; word < 4u; word++) {
            unsigned long value;

            if (!parse_hex_word(text + word * 8u, &value)) {
                return false;
            }
            out->bytes[word * 4u] = (uint8_t)(value & 0xffu);
            out->bytes[word * 4u + 1u] = (uint8_t)((value >> 8) & 0xffu);
            out->bytes[word * 4u + 2u] = (uint8_t)((value >> 16) & 0xffu);
            out->bytes[word * 4u + 3u] = (uint8_t)((value >> 24) & 0xffu);
        }
        return true;
    }
    return false;
}

static bool addr_all_zero(const cp_addr *addr)
{
    size_t i;

    for (i = 0u; i < addr->len; i++) {
        if (addr->bytes[i] != 0u) {
            return false;
        }
    }
    return true;
}

static bool addr_loopback(const cp_addr *addr)
{
    size_t i;

    if (addr->len == 4u) {
        return addr->bytes[0] == 127u;
    }
    for (i = 0u; i < 15u; i++) {
        if (addr->bytes[i] != 0u) {
            break;
        }
    }
    if (i == 15u && addr->bytes[15] == 1u) {
        return true; /* ::1 */
    }
    /* IPv4-mapped ::ffff:127.0.0.0/8 */
    for (i = 0u; i < 10u; i++) {
        if (addr->bytes[i] != 0u) {
            return false;
        }
    }
    return addr->bytes[10] == 0xffu && addr->bytes[11] == 0xffu &&
           addr->bytes[12] == 127u;
}

static bool literal_address_matches(const char *text, const cp_addr *addr)
{
    char copy[64];
    char *zone;
    uint8_t parsed[16];
    size_t len;

    if (text == NULL || strlen(text) >= sizeof(copy)) {
        return false;
    }
    memcpy(copy, text, strlen(text) + 1u);
    zone = strchr(copy, '%');
    if (zone != NULL) {
        *zone = '\0';
    }
    if (addr->len == 4u) {
        struct in_addr v4;

        if (inet_pton(AF_INET, copy, &v4) != 1) {
            return false;
        }
        memcpy(parsed, &v4, 4u);
        len = 4u;
    } else {
        struct in6_addr v6;

        if (inet_pton(AF_INET6, copy, &v6) != 1) {
            return false;
        }
        memcpy(parsed, &v6, 16u);
        len = 16u;
    }
    return len == addr->len && memcmp(parsed, addr->bytes, len) == 0;
}

static bool interface_address_matches(const cp_receiver_listener_config *config,
                                      const cp_addr *addr)
{
    size_t i;

    if (config->interface_addresses != NULL) {
        for (i = 0u; i < config->interface_address_count; i++) {
            if (literal_address_matches(config->interface_addresses[i],
                                        addr)) {
                return true;
            }
        }
        return false;
    }
    if (config->interface_name == NULL) {
        return false;
    }
    {
        struct ifaddrs *list = NULL;
        struct ifaddrs *entry;
        bool match = false;

        if (getifaddrs(&list) != 0) {
            return false;
        }
        for (entry = list; entry != NULL && !match; entry = entry->ifa_next) {
            if (entry->ifa_name == NULL || entry->ifa_addr == NULL ||
                strcmp(entry->ifa_name, config->interface_name) != 0) {
                continue;
            }
            if (entry->ifa_addr->sa_family == AF_INET && addr->len == 4u) {
                const struct sockaddr_in *v4 =
                    (const struct sockaddr_in *)(const void *)entry->ifa_addr;

                match = memcmp(&v4->sin_addr, addr->bytes, 4u) == 0;
            } else if (entry->ifa_addr->sa_family == AF_INET6 &&
                       addr->len == 16u) {
                const struct sockaddr_in6 *v6 =
                    (const struct sockaddr_in6 *)(const void *)entry->ifa_addr;

                match = memcmp(&v6->sin6_addr, addr->bytes, 16u) == 0;
            }
        }
        freeifaddrs(list);
        return match;
    }
}

typedef struct {
    bool opened;
    bool found;
    bool wildcard_seen;
    bool loopback_seen;
    bool specific_seen;
    bool foreign_seen;
    bool inode_overflow;
    unsigned long inodes[CP_RECEIVER_LISTENER_MAX_INODES];
    size_t inode_count;
} listener_scan;

static void add_listener_inode(listener_scan *scan, unsigned long inode)
{
    size_t i;

    if (inode == 0u) {
        return;
    }
    for (i = 0u; i < scan->inode_count; i++) {
        if (scan->inodes[i] == inode) {
            return;
        }
    }
    if (scan->inode_count >= CP_RECEIVER_LISTENER_MAX_INODES) {
        scan->inode_overflow = true;
        return;
    }
    scan->inodes[scan->inode_count++] = inode;
}

static void scan_listener_file(const char *path, uint16_t port,
                               const cp_receiver_listener_config *config,
                               listener_scan *scan)
{
    FILE *file = fopen(path, "r");
    char line[512];

    if (file == NULL) {
        return;
    }
    scan->opened = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        char local[128];
        unsigned state = 0u;
        unsigned long inode = 0u;
        char *colon;
        char *end;
        unsigned long found_port;
        cp_addr addr;

        if (sscanf(line, " %*u: %127s %*s %x %*s %*s %*s %*u %*u %lu",
                   local, &state, &inode) != 3) {
            continue;
        }
        if (state != CP_RECEIVER_LISTENER_STATE_LISTEN) {
            continue;
        }
        colon = strrchr(local, ':');
        if (colon == NULL || colon[1] == '\0') {
            continue;
        }
        end = NULL;
        found_port = strtoul(colon + 1, &end, 16);
        if (end == NULL || *end != '\0' || found_port > 65535u ||
            (uint16_t)found_port != port) {
            continue;
        }
        *colon = '\0';
        if (!parse_proc_addr(local, &addr)) {
            continue;
        }
        scan->found = true;
        if (addr_all_zero(&addr)) {
            scan->wildcard_seen = true;
            add_listener_inode(scan, inode);
        } else if (addr_loopback(&addr)) {
            scan->loopback_seen = true;
        } else if (interface_address_matches(config, &addr)) {
            scan->specific_seen = true;
            add_listener_inode(scan, inode);
        } else {
            scan->foreign_seen = true;
        }
    }
    fclose(file);
}

static bool dirent_is_pid(const char *name)
{
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    for (i = 0u; name[i] != '\0'; i++) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
    }
    return true;
}

static bool read_comm(const char *proc_root, const char *pid, char *name,
                      size_t name_size)
{
    char path[512];
    int fd;
    ssize_t got;
    size_t len;

    if (snprintf(path, sizeof(path), "%s/%s/comm", proc_root, pid) >=
        (int)sizeof(path)) {
        return false;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        return false;
    }
    got = read(fd, name, name_size - 1u);
    close(fd);
    if (got <= 0) {
        return false;
    }
    name[got] = '\0';
    len = strlen(name);
    while (len > 0u && (name[len - 1u] == '\n' || name[len - 1u] == '\r' ||
                        name[len - 1u] == ' ')) {
        name[--len] = '\0';
    }
    return true;
}

/*
 * Bounded /proc/<pid>/fd scan.  readlink() reads the link target without
 * following it; comm is opened with O_NOFOLLOW.  No socket is opened.
 */
static bool owner_lookup(const char *proc_root, unsigned long inode,
                         unsigned max_fds, int *pid_out, char *name,
                         size_t name_size)
{
    DIR *proc;
    struct dirent *pid_entry;
    unsigned pids = 0u;
    unsigned fds = 0u;
    bool found = false;

    proc = opendir(proc_root);
    if (proc == NULL) {
        return false;
    }
    while (!found && (pid_entry = readdir(proc)) != NULL) {
        char fd_dir_path[512];
        DIR *fd_dir;
        struct dirent *fd_entry;

        if (!dirent_is_pid(pid_entry->d_name)) {
            continue;
        }
        if (++pids > CP_RECEIVER_LISTENER_MAX_PIDS) {
            break;
        }
        if (snprintf(fd_dir_path, sizeof(fd_dir_path), "%s/%s/fd",
                     proc_root, pid_entry->d_name) >=
            (int)sizeof(fd_dir_path)) {
            continue;
        }
        fd_dir = opendir(fd_dir_path);
        if (fd_dir == NULL) {
            continue;
        }
        while (!found && (fd_entry = readdir(fd_dir)) != NULL) {
            char target[128];
            char link_path[600];
            ssize_t got;

            if (!dirent_is_pid(fd_entry->d_name)) {
                continue;
            }
            if (++fds > max_fds) {
                break;
            }
            if (snprintf(link_path, sizeof(link_path), "%s/%s",
                         fd_dir_path, fd_entry->d_name) >=
                (int)sizeof(link_path)) {
                continue;
            }
            got = readlink(link_path, target, sizeof(target) - 1u);
            if (got <= 0) {
                continue;
            }
            target[got] = '\0';
            if (strncmp(target, "socket:[", 8u) != 0) {
                continue;
            }
            {
                char *end = NULL;
                unsigned long target_inode =
                    strtoul(target + 8u, &end, 10);

                if (end == NULL || *end != ']' ||
                    target_inode != inode) {
                    continue;
                }
            }
            if (!read_comm(proc_root, pid_entry->d_name, name, name_size)) {
                continue;
            }
            *pid_out = atoi(pid_entry->d_name);
            found = true;
        }
        closedir(fd_dir);
    }
    closedir(proc);
    return found;
}

static bool owner_allowlisted(const char *allowlist, const char *name)
{
    const char *cursor;
    const char *default_list = "AppProjection";

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    if (allowlist == NULL || allowlist[0] == '\0') {
        allowlist = default_list;
    }
    cursor = allowlist;
    while (*cursor != '\0') {
        const char *comma = strchr(cursor, ',');
        const char *end = comma != NULL ? comma : cursor + strlen(cursor);
        const char *start = cursor;

        while (start < end && (*start == ' ' || *start == '\t')) {
            start++;
        }
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        if ((size_t)(end - start) == strlen(name) &&
            strncmp(start, name, (size_t)(end - start)) == 0) {
            return true;
        }
        if (comma == NULL) {
            break;
        }
        cursor = comma + 1;
    }
    return false;
}

cp_receiver_listener_status cp_receiver_listener_check(
    uint16_t port, const cp_receiver_listener_config *config,
    cp_receiver_listener_result *out)
{
    char path[512];
    listener_scan scan;
    unsigned max_fds;
    size_t i;

    if (out == NULL) {
        return CP_RECEIVER_LISTENER_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->status = CP_RECEIVER_LISTENER_ARGUMENT;
    if (config == NULL || port == 0u ||
        config->proc_root == NULL || config->proc_root[0] == '\0') {
        return out->status;
    }
    memset(&scan, 0, sizeof(scan));
    if (snprintf(path, sizeof(path), "%s/net/tcp", config->proc_root) <
        (int)sizeof(path)) {
        scan_listener_file(path, port, config, &scan);
    }
    if (snprintf(path, sizeof(path), "%s/net/tcp6", config->proc_root) <
        (int)sizeof(path)) {
        scan_listener_file(path, port, config, &scan);
    }
    if (!scan.opened) {
        out->status = CP_RECEIVER_LISTENER_IO;
        return out->status;
    }
    if (!scan.found) {
        out->status = CP_RECEIVER_LISTENER_ABSENT;
        return out->status;
    }
    out->found = true;
    out->wildcard = scan.wildcard_seen;
    out->loopback_only = scan.loopback_seen && !scan.wildcard_seen &&
                         !scan.specific_seen && !scan.foreign_seen;
    out->address_allowed = scan.wildcard_seen || scan.specific_seen;
    out->inode = scan.inode_count > 0u ? scan.inodes[0] : 0u;
    if (!out->address_allowed) {
        out->status = CP_RECEIVER_LISTENER_SCOPE;
        return out->status;
    }
    /*
     * Every acceptable socket on the port must have a known, allowlisted
     * owner.  A second wildcard socket owned by something else is not made
     * acceptable by the first one.
     */
    if (scan.inode_overflow || scan.inode_count == 0u) {
        out->status = CP_RECEIVER_LISTENER_OWNER;
        return out->status;
    }
    max_fds = config->max_fd_scan != 0u
                  ? config->max_fd_scan
                  : CP_RECEIVER_LISTENER_DEFAULT_FDS;
    for (i = 0u; i < scan.inode_count; i++) {
        int pid = 0;
        char name[64];

        memset(name, 0, sizeof(name));
        if (!owner_lookup(config->proc_root, scan.inodes[i], max_fds, &pid,
                          name, sizeof(name))) {
            out->owner_allowed = false;
            out->status = CP_RECEIVER_LISTENER_OWNER;
            return out->status;
        }
        if (i == 0u) {
            out->owner_pid = pid;
            memcpy(out->owner_name, name, sizeof(out->owner_name));
            out->owner_name[sizeof(out->owner_name) - 1u] = '\0';
        }
        if (!owner_allowlisted(config->owner_allowlist, name)) {
            memcpy(out->owner_name, name, sizeof(out->owner_name));
            out->owner_name[sizeof(out->owner_name) - 1u] = '\0';
            out->owner_allowed = false;
            out->status = CP_RECEIVER_LISTENER_OWNER;
            return out->status;
        }
    }
    out->owner_allowed = true;
    out->status = CP_RECEIVER_LISTENER_OK;
    return out->status;
}

const char *cp_receiver_policy_status_string(cp_receiver_policy_status status)
{
    switch (status) {
    case CP_RECEIVER_POLICY_OK:
        return "ok";
    case CP_RECEIVER_POLICY_ERR_ARGUMENT:
        return "argument";
    case CP_RECEIVER_POLICY_ERR_NO_SERVICE:
        return "no-service";
    case CP_RECEIVER_POLICY_ERR_AMBIGUOUS:
        return "ambiguous";
    case CP_RECEIVER_POLICY_ERR_PORT:
        return "port";
    case CP_RECEIVER_POLICY_ERR_DEVICE_ID:
        return "device-id";
    case CP_RECEIVER_POLICY_ERR_SOURCE_VERSION:
        return "source-version";
    case CP_RECEIVER_POLICY_ERR_TXT:
        return "txt";
    case CP_RECEIVER_POLICY_ERR_PI:
        return "pi";
    case CP_RECEIVER_POLICY_ERR_RESOLVE:
        return "resolve";
    case CP_RECEIVER_POLICY_ERR_BT_MAC:
        return "bt-mac";
    default:
        return "unknown";
    }
}

const char *cp_receiver_listener_status_string(
    cp_receiver_listener_status status)
{
    switch (status) {
    case CP_RECEIVER_LISTENER_OK:
        return "ok";
    case CP_RECEIVER_LISTENER_ABSENT:
        return "listener-absent";
    case CP_RECEIVER_LISTENER_SCOPE:
        return "listener-scope";
    case CP_RECEIVER_LISTENER_OWNER:
        return "listener-owner";
    case CP_RECEIVER_LISTENER_ARGUMENT:
        return "argument";
    case CP_RECEIVER_LISTENER_IO:
        return "io";
    default:
        return "unknown";
    }
}
