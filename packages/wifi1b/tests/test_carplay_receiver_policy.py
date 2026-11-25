#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Offline tests for receiver policy, listener ownership and lookup.

The DNS-SD backend is a locally generated mock shared object.  No network,
mDNS, D-Bus or hardware access is used.  The test binary is built and run
under AddressSanitizer; the reentrant-callback fixture fails on a UAF.
"""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src/carplay"
TEST = ROOT / "tests/test_carplay_receiver_policy.c"
CC = os.environ.get("CC", "cc")
COMMON = [
    CC,
    "-std=c11",
    "-Wall",
    "-Wextra",
    "-Werror",
    "-pedantic",
    "-Wshadow",
    "-Wstrict-prototypes",
    "-Wmissing-prototypes",
    "-I",
    str(SRC),
]

MOCK_SOURCE = r"""
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef void *DNSServiceRef;
typedef int32_t DNSServiceErrorType;
typedef uint32_t DNSServiceFlags;
typedef void (*DNSServiceBrowseReply)(DNSServiceRef, DNSServiceFlags,
                                      uint32_t, DNSServiceErrorType,
                                      const char *, const char *, const char *,
                                      void *);
typedef void (*DNSServiceResolveReply)(DNSServiceRef, DNSServiceFlags,
                                       uint32_t, DNSServiceErrorType,
                                       const char *, const char *, uint16_t,
                                       uint16_t, const unsigned char *,
                                       void *);

#define MOCK_MAGIC 0x4d4f434bu
#define MOCK_MAX 4
#define MOCK_NAME_MAX 128

typedef struct mock_ref {
    uint32_t magic;
    int kind; /* 1 browse, 2 resolve */
    int fd;
    int wfd;
    int index;
    int delivered;
    uint32_t ifindex;
    DNSServiceBrowseReply browse_cb;
    DNSServiceResolveReply resolve_cb;
    void *ctx;
} mock_ref;

static int g_ready;
static int g_count;
static char g_names[MOCK_MAX][MOCK_NAME_MAX];
static int g_port;
static int g_invalid_first;
static int g_duplicate;
static int g_none;
static int g_no_resolve_reply;

static int env_int(const char *name, int fallback)
{
    const char *value = getenv(name);

    if (value == NULL || value[0] == '\0') {
        return fallback;
    }
    return atoi(value);
}

static const char *env_str(const char *name, const char *fallback)
{
    const char *value = getenv(name);

    return value != NULL && value[0] != '\0' ? value : fallback;
}

static void mock_init(void)
{
    const char *base;
    int i;

    if (g_ready) {
        return;
    }
    g_ready = 1;
    g_count = env_int("CP_MOCK_SERVICES", 1);
    if (g_count < 0) {
        g_count = 0;
    }
    if (g_count > MOCK_MAX) {
        g_count = MOCK_MAX;
    }
    base = env_str("CP_MOCK_NAME", "MockReceiver");
    for (i = 0; i < g_count; i++) {
        if (g_count == 1) {
            snprintf(g_names[i], MOCK_NAME_MAX, "%s", base);
        } else {
            snprintf(g_names[i], MOCK_NAME_MAX, "%s-%d", base, i);
        }
    }
    g_port = env_int("CP_MOCK_PORT", 7000);
    g_invalid_first = env_int("CP_MOCK_INVALID_FIRST", 0);
    g_duplicate = env_int("CP_MOCK_DUPLICATE_ADD", 0);
    g_none = env_int("CP_MOCK_NONE", 0);
    g_no_resolve_reply = env_int("CP_MOCK_NO_RESOLVE_REPLY", 0);
}

static size_t txt_add(unsigned char *buf, size_t off, const char *key,
                      const char *value)
{
    size_t key_len = strlen(key);
    size_t value_len = strlen(value);

    if (key_len + value_len + 1u > 255u ||
        off + key_len + value_len + 2u > 512u) {
        return off;
    }
    buf[off++] = (unsigned char)(key_len + value_len + 1u);
    memcpy(buf + off, key, key_len);
    off += key_len;
    buf[off++] = '=';
    memcpy(buf + off, value, value_len);
    off += value_len;
    return off;
}

static void service_pi(int index, char *out, size_t cap)
{
    const char *base = env_str("CP_MOCK_PI",
                               "0123456789abcdef0123456789abcdef");

    if (g_count <= 1) {
        snprintf(out, cap, "%s", base);
    } else {
        snprintf(out, cap, "%s%d", base, index);
    }
}

static int new_ref(DNSServiceRef *sd_ref, int kind, uint32_t ifindex,
                   const char *name)
{
    int fds[2];
    mock_ref *ref;
    int i;

    if (pipe(fds) != 0) {
        return -1;
    }
    ref = calloc(1, sizeof(*ref));
    if (ref == NULL) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    ref->magic = MOCK_MAGIC;
    ref->kind = kind;
    ref->fd = fds[0];
    ref->wfd = fds[1];
    ref->ifindex = ifindex;
    if (kind == 2) {
        for (i = 0; i < g_count; i++) {
            if (strcmp(g_names[i], name) == 0) {
                ref->index = i;
                break;
            }
        }
    }
    if (write(fds[1], "x", 1) != 1) {
        close(fds[0]);
        close(fds[1]);
        free(ref);
        return -1;
    }
    *sd_ref = ref;
    return 0;
}

int32_t DNSServiceBrowse(DNSServiceRef *sd_ref, uint32_t flags,
                         uint32_t ifindex, const char *regtype,
                         const char *domain, DNSServiceBrowseReply reply,
                         void *context)
{
    mock_ref *ref;
    int32_t result;

    (void)flags;
    (void)regtype;
    (void)domain;
    mock_init();
    result = new_ref(sd_ref, 1, ifindex, "");
    if (result != 0) {
        return result;
    }
    ref = *sd_ref;
    ref->browse_cb = reply;
    ref->ctx = context;
    return 0;
}

#ifndef CP_MOCK_NO_RESOLVE
int32_t DNSServiceResolve(DNSServiceRef *sd_ref, uint32_t flags,
                          uint32_t ifindex, const char *name,
                          const char *regtype, const char *domain,
                          DNSServiceResolveReply reply, void *context)
{
    mock_ref *ref;
    int32_t result;

    (void)flags;
    (void)regtype;
    (void)domain;
    mock_init();
    result = new_ref(sd_ref, 2, ifindex, name);
    if (result != 0) {
        return result;
    }
    ref = *sd_ref;
    ref->resolve_cb = reply;
    ref->ctx = context;
    return 0;
}
#endif

int DNSServiceRefSockFD(DNSServiceRef sd_ref)
{
    mock_ref *ref = sd_ref;

    return ref != NULL ? ref->fd : -1;
}

int32_t DNSServiceProcessResult(DNSServiceRef sd_ref)
{
    mock_ref *ref = sd_ref;
    char scratch[8];
    int i;

    if (ref == NULL || ref->magic != MOCK_MAGIC) {
        return -1;
    }
    (void)read(ref->fd, scratch, sizeof(scratch));
    if (ref->kind == 1) {
        if (ref->delivered) {
            return 0;
        }
        ref->delivered = 1;
        if (g_none) {
            return 0;
        }
        for (i = 0; i < g_count; i++) {
            ref->browse_cb(ref, 0x2u, ref->ifindex, 0, g_names[i],
                           "_airplay._tcp", "local.", ref->ctx);
            if (ref->magic != MOCK_MAGIC) {
                return -1; /* UAF: the host freed the ref in a callback */
            }
            if (g_duplicate && i == 0) {
                ref->browse_cb(ref, 0x2u, ref->ifindex, 0, g_names[i],
                               "_airplay._tcp", "local.", ref->ctx);
                if (ref->magic != MOCK_MAGIC) {
                    return -1;
                }
            }
        }
        return 0;
    }
    if (ref->delivered) {
        return 0;
    }
    ref->delivered = 1;
    if (g_no_resolve_reply) {
        return 0;
    }
    {
        unsigned char txt[512];
        char pi[128];
        char fullname[256];
        size_t length = 0u;

        (void)snprintf(fullname, sizeof(fullname),
                       "%s._airplay._tcp.local.", g_names[ref->index]);
        length = txt_add(txt, length, "deviceid",
                         env_str("CP_MOCK_DEVICEID", "AA:BB:CC:DD:EE:01"));
        if (!(g_invalid_first && ref->index == 0)) {
            service_pi(ref->index, pi, sizeof(pi));
            length = txt_add(txt, length, "pi", pi);
        }
        length = txt_add(txt, length, "srcvers",
                         env_str("CP_MOCK_SRCVERS", "320.17.6"));
        ref->resolve_cb(ref, 0u, ref->ifindex, 0, fullname, "mock.local.",
                        htons((uint16_t)g_port), (uint16_t)length, txt,
                        ref->ctx);
        if (ref->magic != MOCK_MAGIC) {
            return -1; /* UAF: the host freed the ref in a callback */
        }
    }
    return 0;
}

void DNSServiceRefDeallocate(DNSServiceRef sd_ref)
{
    mock_ref *ref = sd_ref;

    if (ref == NULL || ref->magic != MOCK_MAGIC) {
        return;
    }
    ref->magic = 0xdeadbeefu;
    close(ref->fd);
    close(ref->wfd);
    free(ref);
}
"""

with tempfile.TemporaryDirectory(prefix="cp-receiver-policy-",
                                 dir="/tmp/opencode") as tmp_name:
    tmp = Path(tmp_name)
    mock_source = tmp / "mock_dns_sd.c"
    mock_source.write_text(MOCK_SOURCE)
    mock = tmp / "libdns_sd_mock.so"
    missing = tmp / "libdns_sd_missing.so"
    subprocess.run(
        [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-fPIC", "-shared", "-fsanitize=address", "-fno-omit-frame-pointer",
         "-O1", "-o", str(mock), str(mock_source)],
        check=True, timeout=120,
    )
    subprocess.run(
        [CC, "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-fPIC", "-shared", "-DCP_MOCK_NO_RESOLVE",
         "-fsanitize=address", "-fno-omit-frame-pointer",
         "-O1", "-o", str(missing), str(mock_source)],
        check=True, timeout=120,
    )

    pin_root = tmp / "pin-root"
    (pin_root / "app" / "lib").mkdir(parents=True)
    (pin_root / "app" / "lib" / "libdns_sd.so").write_bytes(
        b"deliberately not the pinned library"
    )
    empty_root = tmp / "empty-root"
    empty_root.mkdir()

    binary = tmp / "test-receiver-policy"
    subprocess.run(
        [*COMMON, "-fsanitize=address", "-fno-omit-frame-pointer", "-O1",
         str(SRC / "cp_receiver_lookup.c"),
         str(SRC / "cp_receiver_policy.c"),
         str(SRC / "cp_oem_receiver.c"),
         str(SRC / "cp_wire.c"),
         str(TEST), "-ldl", "-o", str(binary)],
        check=True, timeout=120,
    )
    env = dict(os.environ)
    env["ASAN_OPTIONS"] = "detect_leaks=1:abort_on_error=1"
    subprocess.run(
        [str(binary), str(mock), str(missing), str(pin_root),
         str(empty_root)],
        check=True, timeout=180, env=env,
    )

print("PASS: receiver policy, listener and ASan lookup tests")
