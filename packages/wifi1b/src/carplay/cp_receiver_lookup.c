/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * Bounded _airplay._tcp lookup using the target libdns_sd.
 *
 * Every distinct (interface_index, service_name) browse instance is resolved
 * at most once, up to the caller's max_services.  Reply callbacks never
 * deallocate the reference being processed: they fill a fixed slot, mark it
 * done and append the ref to a fixed-size pending list.  The list is drained
 * only after DNSServiceProcessResult has returned.
 */

#define _POSIX_C_SOURCE 200809L

#include "cp_receiver_lookup.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define CP_RECEIVER_LOOKUP_POLL_MS 200
#define CP_RECEIVER_LOOKUP_DEFAULT_SETTLE_MS 500u
#define CP_RECEIVER_LOOKUP_DEFAULT_SERVICES 2u
#define CP_RECEIVER_LOOKUP_PENDING_MAX (CP_OEM_RECEIVER_MAX_SERVICES + 1u)
#define CP_RECEIVER_LOOKUP_BROWSE_ADD 0x2u
#define CP_RECEIVER_LOOKUP_MAX_PATH 512u

typedef void *cp_dnssd_ref;
typedef void (*cp_dnssd_browse_reply)(cp_dnssd_ref sd_ref, uint32_t flags,
                                      uint32_t interface_index,
                                      int32_t error_code,
                                      const char *service_name,
                                      const char *regtype,
                                      const char *reply_domain,
                                      void *context);
typedef void (*cp_dnssd_resolve_reply)(cp_dnssd_ref sd_ref, uint32_t flags,
                                       uint32_t interface_index,
                                       int32_t error_code,
                                       const char *fullname,
                                       const char *hosttarget, uint16_t port,
                                       uint16_t txt_len,
                                       const unsigned char *txt_record,
                                       void *context);
typedef int32_t (*cp_dnssd_browse_fn)(cp_dnssd_ref *sd_ref, uint32_t flags,
                                      uint32_t interface_index,
                                      const char *regtype, const char *domain,
                                      cp_dnssd_browse_reply reply,
                                      void *context);
typedef int32_t (*cp_dnssd_resolve_fn)(cp_dnssd_ref *sd_ref, uint32_t flags,
                                       uint32_t interface_index,
                                       const char *name,
                                       const char *regtype,
                                       const char *domain,
                                       cp_dnssd_resolve_reply reply,
                                       void *context);
typedef int (*cp_dnssd_sockfd_fn)(cp_dnssd_ref sd_ref);
typedef int32_t (*cp_dnssd_process_fn)(cp_dnssd_ref sd_ref);
typedef void (*cp_dnssd_dealloc_fn)(cp_dnssd_ref sd_ref);

typedef union {
    void *object;
    cp_dnssd_browse_fn browse;
    cp_dnssd_resolve_fn resolve;
    cp_dnssd_sockfd_fn sockfd;
    cp_dnssd_process_fn process;
    cp_dnssd_dealloc_fn dealloc;
} receiver_dns_symbol;

typedef struct receiver_lookup receiver_lookup;

typedef struct {
    bool used;
    bool done;
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    uint32_t interface_index;
    cp_dnssd_ref ref;
    receiver_lookup *owner;
    cp_receiver_candidate_state state;
    uint16_t port;
    char device_id[CP_OEM_RECEIVER_MAX_DEVICE_ID];
    char source_version[CP_OEM_RECEIVER_MAX_SRCVERS];
    uint8_t pi_sha256[CP_OEM_RECEIVER_SHA256_LEN];
    size_t pi_len;
    bool have_pi;
} receiver_resolve_slot;

typedef struct {
    bool used;
    bool active;
    char service_name[CP_OEM_RECEIVER_MAX_SERVICE_NAME];
    uint32_t interface_index;
    size_t slot; /* index into resolves[] */
} receiver_browse_instance;

struct receiver_lookup {
    const cp_receiver_lookup_config *config;
    cp_receiver_stop_fn stop_fn;
    void *stop_context;
    void *dl;
    cp_dnssd_browse_fn browse;
    cp_dnssd_resolve_fn resolve;
    cp_dnssd_sockfd_fn sockfd;
    cp_dnssd_process_fn process;
    cp_dnssd_dealloc_fn dealloc;
    cp_dnssd_ref browse_ref;
    receiver_resolve_slot resolves[CP_OEM_RECEIVER_MAX_SERVICES];
    receiver_browse_instance instances[CP_OEM_RECEIVER_MAX_SERVICES];
    uint32_t max_services;
    uint32_t instance_count;
    uint32_t overflow_count;
    uint32_t resolved_count;
    uint32_t rejected_count;
    bool too_many;
    bool dns_error;
    uint64_t last_event_ms;
    uint64_t first_valid_ms;
    cp_dnssd_ref pending[CP_RECEIVER_LOOKUP_PENDING_MAX];
    size_t pending_count;
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0u;
    }
    return (uint64_t)now.tv_sec * 1000u +
           (uint64_t)now.tv_nsec / 1000000u;
}

static uint16_t host_port(uint16_t port)
{
    return (uint16_t)((uint16_t)(port >> 8) | (uint16_t)(port << 8));
}

static bool stop_requested(const receiver_lookup *lookup)
{
    return lookup->stop_fn != NULL &&
           lookup->stop_fn(lookup->stop_context) != 0;
}

static void defer_dealloc(receiver_lookup *lookup, cp_dnssd_ref ref)
{
    size_t i;

    if (ref == NULL) {
        return;
    }
    for (i = 0u; i < lookup->pending_count; i++) {
        if (lookup->pending[i] == ref) {
            return;
        }
    }
    if (lookup->pending_count < CP_RECEIVER_LOOKUP_PENDING_MAX) {
        lookup->pending[lookup->pending_count++] = ref;
    }
}

/* Only call this after DNSServiceProcessResult has returned. */
static void drain_pending(receiver_lookup *lookup)
{
    size_t i;

    for (i = 0u; i < lookup->pending_count; i++) {
        cp_dnssd_ref ref = lookup->pending[i];
        size_t slot;

        for (slot = 0u; slot < CP_OEM_RECEIVER_MAX_SERVICES; slot++) {
            if (lookup->resolves[slot].ref == ref) {
                lookup->resolves[slot].ref = NULL;
            }
        }
        if (lookup->dealloc != NULL) {
            lookup->dealloc(ref);
        }
    }
    lookup->pending_count = 0u;
}

static void stop_lookup(receiver_lookup *lookup)
{
    size_t i;

    if (lookup->dealloc == NULL) {
        return;
    }
    drain_pending(lookup);
    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        if (lookup->resolves[i].ref != NULL) {
            lookup->dealloc(lookup->resolves[i].ref);
            lookup->resolves[i].ref = NULL;
        }
    }
    if (lookup->browse_ref != NULL) {
        lookup->dealloc(lookup->browse_ref);
        lookup->browse_ref = NULL;
    }
}

static void touch_event(receiver_lookup *lookup)
{
    lookup->last_event_ms = monotonic_ms();
}

static receiver_resolve_slot *find_free_slot(receiver_lookup *lookup)
{
    size_t i;

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        if (!lookup->resolves[i].used) {
            return &lookup->resolves[i];
        }
    }
    return NULL;
}

static void resolve_reply(cp_dnssd_ref sd_ref, uint32_t flags,
                          uint32_t interface_index, int32_t error_code,
                          const char *fullname, const char *hosttarget,
                          uint16_t port, uint16_t txt_len,
                          const unsigned char *txt_record, void *context)
{
    receiver_resolve_slot *slot = context;
    receiver_lookup *lookup;
    cp_oem_receiver_txt txt;
    uint16_t observed_port;

    (void)flags;
    (void)fullname; /* Escaped instance names are not portable across backends;
                     * the resolve request/ref already binds this slot. */
    (void)hosttarget;
    if (slot == NULL || slot->done || slot->owner == NULL) {
        return;
    }
    lookup = slot->owner;
    if (slot->ref != NULL && sd_ref != slot->ref) {
        return;
    }
    slot->done = true;
    /* Defer even if slot->ref was not assigned yet (a synchronous mock
     * callback); the pointer is cleared by drain_pending or stop_lookup. */
    defer_dealloc(lookup, sd_ref);
    touch_event(lookup);
    if (error_code != 0) {
        slot->state = CP_RECEIVER_CANDIDATE_RESOLVE_ERROR;
        lookup->rejected_count++;
        return;
    }
    observed_port = host_port(port);
    if (cp_oem_receiver_validate_srv(interface_index, slot->interface_index,
                                     observed_port, slot->service_name,
                                     strlen(slot->service_name)) !=
        CP_OEM_RECEIVER_OK) {
        slot->state = CP_RECEIVER_CANDIDATE_SRV;
        lookup->rejected_count++;
        return;
    }
    memset(&txt, 0, sizeof(txt));
    if (cp_oem_receiver_parse_txt(txt_record, txt_len, &txt) !=
        CP_OEM_RECEIVER_OK) {
        slot->state = CP_RECEIVER_CANDIDATE_TXT;
        lookup->rejected_count++;
        return;
    }
    slot->port = observed_port;
    memcpy(slot->device_id, txt.device_id,
           strlen(txt.device_id) + 1u);
    memcpy(slot->source_version, txt.source_version,
           strlen(txt.source_version) + 1u);
    memcpy(slot->pi_sha256, txt.pi_sha256, sizeof(slot->pi_sha256));
    slot->pi_len = txt.pi_len;
    slot->have_pi = txt.have_pi;
    slot->state = CP_RECEIVER_CANDIDATE_VALID;
    lookup->resolved_count++;
    if (lookup->first_valid_ms == 0u) {
        lookup->first_valid_ms = lookup->last_event_ms;
    }
}

static receiver_browse_instance *find_instance(receiver_lookup *lookup,
                                               uint32_t interface_index,
                                               const char *service_name)
{
    size_t i;

    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        if (lookup->instances[i].used &&
            lookup->instances[i].interface_index == interface_index &&
            strcmp(lookup->instances[i].service_name, service_name) == 0) {
            return &lookup->instances[i];
        }
    }
    return NULL;
}

static void start_resolve(receiver_lookup *lookup,
                          receiver_browse_instance *instance)
{
    receiver_resolve_slot *slot = find_free_slot(lookup);
    int32_t result;

    if (slot == NULL) {
        lookup->too_many = true;
        return;
    }
    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->owner = lookup;
    slot->interface_index = instance->interface_index;
    memcpy(slot->service_name, instance->service_name,
           strlen(instance->service_name) + 1u);
    result = lookup->resolve(
        &slot->ref, 0u, instance->interface_index, instance->service_name,
        CP_OEM_RECEIVER_SERVICE_TYPE, CP_OEM_RECEIVER_SERVICE_DOMAIN,
        resolve_reply, slot);
    if (result != 0) {
        slot->ref = NULL;
        slot->done = true;
        slot->state = CP_RECEIVER_CANDIDATE_RESOLVE_ERROR;
        lookup->rejected_count++;
        touch_event(lookup);
    }
    instance->slot = (size_t)(slot - lookup->resolves);
}

static void browse_reply(cp_dnssd_ref sd_ref, uint32_t flags,
                         uint32_t interface_index, int32_t error_code,
                         const char *service_name, const char *regtype,
                         const char *reply_domain, void *context)
{
    receiver_lookup *lookup = context;
    receiver_browse_instance *instance;
    size_t name_len;
    size_t i;
    bool add = (flags & CP_RECEIVER_LOOKUP_BROWSE_ADD) != 0u;

    (void)sd_ref;
    (void)regtype;
    (void)reply_domain;
    if (lookup == NULL) {
        return;
    }
    if (error_code != 0) {
        lookup->dns_error = true;
        return;
    }
    if (service_name == NULL) {
        lookup->dns_error = true;
        return;
    }
    name_len = strlen(service_name);
    if (name_len == 0u ||
        name_len >= CP_OEM_RECEIVER_MAX_SERVICE_NAME) {
        lookup->dns_error = true;
        return;
    }
    instance = find_instance(lookup, interface_index, service_name);
    if (instance != NULL) {
        /* Duplicate Add or a Remove for the same identity.  Neither is
         * ambiguity; a duplicate Add never starts a second resolve. */
        instance->active = add;
        touch_event(lookup);
        return;
    }
    if (!add) {
        return;
    }
    touch_event(lookup);
    if (lookup->instance_count >= lookup->max_services ||
        lookup->instance_count >= CP_OEM_RECEIVER_MAX_SERVICES) {
        lookup->too_many = true;
        lookup->overflow_count++;
        return;
    }
    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        if (!lookup->instances[i].used) {
            instance = &lookup->instances[i];
            break;
        }
    }
    if (instance == NULL) {
        lookup->too_many = true;
        lookup->overflow_count++;
        return;
    }
    memset(instance, 0, sizeof(*instance));
    instance->used = true;
    instance->active = true;
    instance->interface_index = interface_index;
    memcpy(instance->service_name, service_name, name_len + 1u);
    lookup->instance_count++;
    start_resolve(lookup, instance);
}

static cp_receiver_lookup_status join_path(const char *root, const char *rel,
                                           char *out, size_t out_len)
{
    size_t root_len;
    size_t rel_len;
    bool need_slash;

    if (root == NULL || rel == NULL || out == NULL || out_len == 0u) {
        return CP_RECEIVER_LOOKUP_ARGUMENT;
    }
    root_len = strlen(root);
    rel_len = strlen(rel);
    need_slash = root_len > 0u && root[root_len - 1u] != '/';
    if (root_len + (need_slash ? 1u : 0u) + rel_len + 1u > out_len) {
        return CP_RECEIVER_LOOKUP_ARGUMENT;
    }
    memcpy(out, root, root_len);
    if (need_slash) {
        out[root_len++] = '/';
    }
    memcpy(out + root_len, rel, rel_len + 1u);
    return CP_RECEIVER_LOOKUP_OK;
}

static cp_receiver_lookup_status load_backend(receiver_lookup *lookup)
{
    receiver_dns_symbol symbol;
    const char *root;
    char pinned_path[CP_RECEIVER_LOOKUP_MAX_PATH];
    const char *load_path;
    bool allow_unpinned;
    bool explicit_override;

    root = lookup->config->backend_root;
    if (root == NULL || root[0] == '\0') {
        root = "/";
    }
    allow_unpinned = lookup->config->allow_unpinned_backend;
    explicit_override = allow_unpinned &&
                        lookup->config->backend_library != NULL &&
                        lookup->config->backend_library[0] != '\0';
    pinned_path[0] = '\0';
    if (!allow_unpinned) {
        if (cp_oem_receiver_verify_files(root, &cp_oem_receiver_dns_sd_pin,
                                         1u, NULL) != CP_OEM_RECEIVER_OK) {
            return CP_RECEIVER_LOOKUP_BACKEND_PIN;
        }
        if (join_path(root, cp_oem_receiver_dns_sd_pin.relative_path,
                      pinned_path, sizeof(pinned_path)) !=
            CP_RECEIVER_LOOKUP_OK) {
            return CP_RECEIVER_LOOKUP_BACKEND_PIN;
        }
    }
    if (allow_unpinned && lookup->config->backend_library != NULL &&
        lookup->config->backend_library[0] != '\0') {
        load_path = lookup->config->backend_library;
    } else if (pinned_path[0] != '\0') {
        load_path = pinned_path;
    } else {
        /* Diagnostic fallback only; the runtime path never reaches this. */
        load_path = "libdns_sd.so";
    }
    lookup->dl = dlopen(load_path, RTLD_NOW | RTLD_LOCAL);
    if (lookup->dl == NULL && allow_unpinned && !explicit_override &&
        strcmp(load_path, "libdns_sd.so") != 0) {
        lookup->dl = dlopen("libdns_sd.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (lookup->dl == NULL) {
        return CP_RECEIVER_LOOKUP_NO_BACKEND;
    }
    symbol.object = dlsym(lookup->dl, "DNSServiceBrowse");
    lookup->browse = symbol.browse;
    symbol.object = dlsym(lookup->dl, "DNSServiceResolve");
    lookup->resolve = symbol.resolve;
    symbol.object = dlsym(lookup->dl, "DNSServiceRefSockFD");
    lookup->sockfd = symbol.sockfd;
    symbol.object = dlsym(lookup->dl, "DNSServiceProcessResult");
    lookup->process = symbol.process;
    symbol.object = dlsym(lookup->dl, "DNSServiceRefDeallocate");
    lookup->dealloc = symbol.dealloc;
    if (lookup->browse == NULL || lookup->resolve == NULL ||
        lookup->sockfd == NULL || lookup->process == NULL ||
        lookup->dealloc == NULL) {
        dlclose(lookup->dl);
        lookup->dl = NULL;
        lookup->browse = NULL;
        lookup->resolve = NULL;
        lookup->sockfd = NULL;
        lookup->process = NULL;
        lookup->dealloc = NULL;
        return CP_RECEIVER_LOOKUP_NO_BACKEND;
    }
    return CP_RECEIVER_LOOKUP_OK;
}

/*
 * Process at most one ready reference per call so a resolve callback can
 * start another resolve without invalidating the iteration.
 */
static int poll_lookup(receiver_lookup *lookup, int timeout_ms)
{
    struct pollfd fds[1u + CP_OEM_RECEIVER_MAX_SERVICES];
    cp_dnssd_ref refs[1u + CP_OEM_RECEIVER_MAX_SERVICES];
    size_t count = 0u;
    size_t i;
    int ready;

    if (lookup->browse_ref != NULL) {
        int fd = lookup->sockfd(lookup->browse_ref);

        if (fd >= 0) {
            fds[count].fd = fd;
            fds[count].events = POLLIN;
            fds[count].revents = 0;
            refs[count] = lookup->browse_ref;
            count++;
        }
    }
    for (i = 0u; i < CP_OEM_RECEIVER_MAX_SERVICES; i++) {
        receiver_resolve_slot *slot = &lookup->resolves[i];
        int fd;

        if (!slot->used || slot->ref == NULL || slot->done) {
            continue;
        }
        fd = lookup->sockfd(slot->ref);
        if (fd < 0) {
            continue;
        }
        fds[count].fd = fd;
        fds[count].events = POLLIN;
        fds[count].revents = 0;
        refs[count] = slot->ref;
        count++;
    }
    if (count == 0u) {
        return 0;
    }
    ready = poll(fds, (nfds_t)count, timeout_ms);
    if (ready < 0) {
        return errno == EINTR ? 0 : -1;
    }
    if (ready == 0) {
        return 0;
    }
    for (i = 0u; i < count; i++) {
        if ((fds[i].revents & (POLLIN | POLLERR | POLLHUP)) != 0) {
            int32_t result = lookup->process(refs[i]);

            if (result != 0) {
                lookup->dns_error = true;
            }
            return 1;
        }
    }
    return 0;
}

cp_receiver_lookup_status cp_receiver_lookup_live(
    const cp_receiver_lookup_config *config, cp_receiver_stop_fn stop_fn,
    void *stop_context, cp_receiver_lookup_result *out)
{
    receiver_lookup lookup;
    cp_receiver_lookup_status status;
    cp_receiver_lookup_status final_status = CP_RECEIVER_LOOKUP_OK;
    uint64_t start_ms;
    uint64_t deadline_ms;
    uint64_t settle_ms;
    uint32_t max_services;
    int32_t result;

    if (config == NULL || out == NULL) {
        return CP_RECEIVER_LOOKUP_ARGUMENT;
    }
    if (config->interface_name[0] == '\0' || config->interface_index == 0u ||
        config->timeout_ms == 0u) {
        return CP_RECEIVER_LOOKUP_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    memset(&lookup, 0, sizeof(lookup));
    lookup.config = config;
    lookup.stop_fn = stop_fn;
    lookup.stop_context = stop_context;
    settle_ms = config->settle_ms != 0u
                    ? config->settle_ms
                    : CP_RECEIVER_LOOKUP_DEFAULT_SETTLE_MS;
    if (settle_ms > config->timeout_ms) {
        settle_ms = config->timeout_ms;
    }
    max_services = config->max_services != 0u
                       ? config->max_services
                       : CP_RECEIVER_LOOKUP_DEFAULT_SERVICES;
    if (max_services > CP_OEM_RECEIVER_MAX_SERVICES) {
        max_services = CP_OEM_RECEIVER_MAX_SERVICES;
    }
    lookup.max_services = max_services;

    status = load_backend(&lookup);
    if (status != CP_RECEIVER_LOOKUP_OK) {
        return status;
    }
    result = lookup.browse(&lookup.browse_ref, 0u, config->interface_index,
                           CP_OEM_RECEIVER_SERVICE_TYPE,
                           CP_OEM_RECEIVER_SERVICE_DOMAIN, browse_reply,
                           &lookup);
    if (result != 0) {
        lookup.browse_ref = NULL;
        stop_lookup(&lookup);
        if (lookup.dl != NULL) {
            dlclose(lookup.dl);
        }
        return CP_RECEIVER_LOOKUP_DNS;
    }

    start_ms = monotonic_ms();
    deadline_ms = start_ms + config->timeout_ms;
    for (;;) {
        uint64_t now = monotonic_ms();
        int timeout;
        int ready;

        if (stop_requested(&lookup)) {
            final_status = CP_RECEIVER_LOOKUP_STOPPED;
            break;
        }
        if (lookup.dns_error) {
            final_status = CP_RECEIVER_LOOKUP_DNS;
            break;
        }
        if (lookup.too_many) {
            final_status = CP_RECEIVER_LOOKUP_AMBIGUOUS;
            break;
        }
        if (lookup.first_valid_ms != 0u &&
            now - lookup.last_event_ms >= settle_ms) {
            break;
        }
        if (now >= deadline_ms) {
            break;
        }
        timeout = CP_RECEIVER_LOOKUP_POLL_MS;
        if (now + (uint64_t)timeout > deadline_ms) {
            timeout = (int)(deadline_ms - now);
        }
        ready = poll_lookup(&lookup, timeout);
        drain_pending(&lookup);
        if (ready < 0) {
            lookup.dns_error = true;
        }
    }
    if (lookup.dns_error) {
        final_status = CP_RECEIVER_LOOKUP_DNS;
    } else if (lookup.too_many) {
        final_status = CP_RECEIVER_LOOKUP_AMBIGUOUS;
    } else if (final_status == CP_RECEIVER_LOOKUP_OK) {
        /* A started resolve that never completed leaves the candidate set
         * incomplete; refuse instead of validating partial evidence. */
        size_t slot;

        for (slot = 0u; slot < CP_OEM_RECEIVER_MAX_SERVICES; slot++) {
            if (lookup.resolves[slot].used && !lookup.resolves[slot].done) {
                final_status = CP_RECEIVER_LOOKUP_TIMEOUT;
                break;
            }
        }
    }
    if (final_status == CP_RECEIVER_LOOKUP_OK &&
        lookup.resolved_count == 0u) {
        if (lookup.instance_count == 0u && lookup.overflow_count == 0u) {
            final_status = CP_RECEIVER_LOOKUP_NO_SERVICE;
        } else if (lookup.rejected_count == 0u) {
            /* Instances appeared but a started resolve never completed.
             * The earlier all-done check catches this; keep the distinct
             * timeout for the partial-evidence case. */
            final_status = CP_RECEIVER_LOOKUP_TIMEOUT;
        }
        /* Otherwise every candidate was evaluated and rejected.  Return OK
         * so policy can report the specific SRV/TXT/PI reason instead of
         * collapsing it into a generic timeout. */
    }

    out->service_count = lookup.instance_count + lookup.overflow_count;
    out->resolved_count = lookup.resolved_count;
    out->rejected_count = lookup.rejected_count;
    out->ambiguous = lookup.too_many;
    if (final_status == CP_RECEIVER_LOOKUP_OK) {
        size_t slot;
        size_t next = 0u;

        for (slot = 0u; slot < CP_OEM_RECEIVER_MAX_SERVICES; slot++) {
            if (!lookup.resolves[slot].used || !lookup.resolves[slot].done) {
                continue;
            }
            out->services[next].interface_index =
                lookup.resolves[slot].interface_index;
            out->services[next].port = lookup.resolves[slot].port;
            out->services[next].pi_len = lookup.resolves[slot].pi_len;
            out->services[next].have_pi = lookup.resolves[slot].have_pi;
            out->services[next].state = lookup.resolves[slot].state;
            memcpy(out->services[next].pi_sha256,
                   lookup.resolves[slot].pi_sha256,
                   sizeof(out->services[next].pi_sha256));
            memcpy(out->services[next].service_name,
                   lookup.resolves[slot].service_name,
                   strlen(lookup.resolves[slot].service_name) + 1u);
            memcpy(out->services[next].device_id,
                   lookup.resolves[slot].device_id,
                   strlen(lookup.resolves[slot].device_id) + 1u);
            memcpy(out->services[next].source_version,
                   lookup.resolves[slot].source_version,
                   strlen(lookup.resolves[slot].source_version) + 1u);
            next++;
        }
    } else {
        memset(out->services, 0, sizeof(out->services));
    }
    stop_lookup(&lookup);
    if (lookup.dl != NULL) {
        dlclose(lookup.dl);
    }
    return final_status;
}

const char *cp_receiver_lookup_status_string(cp_receiver_lookup_status status)
{
    switch (status) {
    case CP_RECEIVER_LOOKUP_OK:
        return "ok";
    case CP_RECEIVER_LOOKUP_NO_SERVICE:
        return "no-service";
    case CP_RECEIVER_LOOKUP_TIMEOUT:
        return "timeout";
    case CP_RECEIVER_LOOKUP_AMBIGUOUS:
        return "ambiguous";
    case CP_RECEIVER_LOOKUP_DNS:
        return "dns";
    case CP_RECEIVER_LOOKUP_STOPPED:
        return "stopped";
    case CP_RECEIVER_LOOKUP_ARGUMENT:
        return "argument";
    case CP_RECEIVER_LOOKUP_NO_BACKEND:
        return "no-backend";
    case CP_RECEIVER_LOOKUP_BACKEND_PIN:
        return "backend-pin";
    default:
        return "unknown";
    }
}

const char *cp_receiver_candidate_state_string(
    cp_receiver_candidate_state state)
{
    switch (state) {
    case CP_RECEIVER_CANDIDATE_UNRESOLVED:
        return "unresolved";
    case CP_RECEIVER_CANDIDATE_VALID:
        return "valid";
    case CP_RECEIVER_CANDIDATE_SRV:
        return "srv";
    case CP_RECEIVER_CANDIDATE_TXT:
        return "txt";
    case CP_RECEIVER_CANDIDATE_RESOLVE_ERROR:
        return "resolve-error";
    default:
        return "unknown";
    }
}
