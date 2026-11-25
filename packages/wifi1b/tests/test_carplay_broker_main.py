#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
# SPDX-License-Identifier: AGPL-3.0-only
"""Fixture-only tests for the experimental broker helper front end.

No network, D-Bus, mDNS or hardware access: the DNS-SD backend is a locally
generated mock shared object and the listener is a loopback/wildcard socket
owned by this test process.
"""
import hashlib
import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "src"
BROKER = ROOT / "out/carplay-broker/carplay-broker"
subprocess.run(["make", "-C", str(SRC), "carplay-broker"],
               check=True, timeout=120, stdout=subprocess.DEVNULL)

# Placeholder identity only. Never put an owner's vehicle MAC/device ID in a
# tracked fixture; those belong in the gitignored owner config.
IDENTITY = """\
service_name=SCOURGE-iAP
name=Ioniq 5
model=IONIQ5
manufacturer=Hyundai
serial=TEST-SERIAL
firmware=TEST-FW
hardware=TEST-HW
eap_name=TEST-EAP
language=en
languages=en
transport_name=SCOURGE-TEST
bt_mac=AA:BB:CC:DD:EE:FF
device_id=AA:BB:CC:DD:EE:01
ap_ssid=IONIQ5-CarPlay
ap_psk=test-passphrase
ap_channel=6
ap_security=WPA2
link_local=fe80::1
airplay_port=0
consent=1
usb_active=0
"""

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
    int kind;
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
                return -1;
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
            return -1;
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


def build_mock(path: Path, source: Path) -> None:
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-fPIC", "-shared", "-O2", "-o", str(path), str(source)],
        check=True, timeout=120,
    )


def run_check(broker, config, status, port, env_extra, mock_lib,
              name="MockReceiver",
              pi="policy-test-pi-0123456789abcdef",
              device_id="AA:BB:CC:DD:EE:01",
              listener_owner=None, extra_args=None):
    env = dict(os.environ)
    env.update(env_extra)
    env["CP_MOCK_PORT"] = str(port)
    env["CP_MOCK_NAME"] = name
    env["CP_MOCK_PI"] = pi
    env["CP_MOCK_DEVICEID"] = device_id
    args = [str(broker), "--check-receiver", "--config", str(config),
            "--status", str(status), "--receiver-interface", "lo",
            "--receiver-timeout-ms", "3000", "--receiver-settle-ms", "100",
            "--dns-sd-library", str(mock_lib),
            "--allow-unpinned-dns-sd"]
    if listener_owner is not None:
        args += ["--receiver-listener-owner", listener_owner]
    if extra_args:
        args += extra_args
    return subprocess.run(args, capture_output=True, text=True, timeout=20,
                          env=env)


def load_identity(**overrides):
    text = IDENTITY
    for key, value in overrides.items():
        needle = f"{key}="
        lines = []
        for line in text.splitlines():
            if line.startswith(needle):
                line = f"{key}={value}"
            lines.append(line)
        text = "\n".join(lines) + "\n"
    return text


with tempfile.TemporaryDirectory(prefix="broker-main-", dir="/tmp/opencode") as d:
    tmp = Path(d)
    usb_empty = tmp / "usb-empty"
    usb_empty.mkdir()
    config = tmp / "snapshot.conf"
    config.write_text(IDENTITY.replace("ap_psk=test-passphrase",
                                       r'ap_psk=test\"passphrase'))
    snapshot = tmp / "ap.json"
    snapshot.touch(mode=0o600)
    good = dict(app="wifi1b", mode="carplay", observations_fresh=True,
                ap_active=True, ap_network_ready=True, applying=False,
                ap_ssid="IONIQ5-CarPlay", carplay_ssid="IONIQ5-CarPlay",
                carplay_psk=r'test\"passphrase', ap_channel=6,
                ap_security="WPA2-PSK", ap_linklocal="fe80::1%wlan0",
                updated_ms=int(time.time()) * 1000)

    def check_snapshot(text, expected, file=snapshot):
        if file == snapshot:
            snapshot.write_text(text)
        result = subprocess.run([str(BROKER), "--check-ap", "--config",
                                 str(config), "--status", str(file)],
                                capture_output=True, text=True, timeout=10)
        assert result.returncode == expected, result
        assert "passphrase" not in result.stdout + result.stderr

    check_snapshot(json.dumps(good), 0)
    for replacement in (dict(updated_ms=good["updated_ms"] - 6000),
                        dict(updated_ms=good["updated_ms"] + 60000),
                        dict(carplay_psk="another-passphrase"),
                        dict(carplay_ssid="Other AP"), dict(ap_active=False),
                        dict(applying=True), dict(ap_channel=262)):
        check_snapshot(json.dumps(good | replacement), 2)
    check_snapshot(json.dumps(good)[:-1], 2)
    check_snapshot('{"ap_active":true,' + json.dumps(good)[1:], 2)
    snapshot.chmod(0o644)
    check_snapshot(json.dumps(good), 2)
    snapshot.chmod(0o600)
    link = tmp / "status-link"
    link.symlink_to(snapshot)
    check_snapshot("", 2, link)

    # Missing config is a clean fail-closed exit.
    r = subprocess.run([str(BROKER), "--config", str(tmp / "missing"),
                        "--broker-status", str(tmp / "s0")],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r

    # Incomplete identity is reported without opening the RX socket or bus.
    cfg = tmp / "incomplete.conf"
    cfg.write_text("service_name=SCOURGE-iAP\nname=only\n")
    status = tmp / "s1"
    r = subprocess.run([str(BROKER), "--config", str(cfg),
                        "--broker-status", str(status)],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r
    text = status.read_text()
    assert '"state":"identity_incomplete"' in text, text
    assert '"ok":false' in text, text

    # An unknown security value must not silently become WPA2.
    cfg2 = tmp / "badsec.conf"
    cfg2.write_text(IDENTITY.replace("ap_security=WPA2", "ap_security=BOGUS"))
    status2 = tmp / "s2"
    r = subprocess.run([str(BROKER), "--config", str(cfg2),
                        "--broker-status", str(status2)],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r
    text = status2.read_text()
    assert '"state":"identity_incomplete"' in text, text
    assert '"ok":false' in text, text
    assert "unsupported AP security" in text, text

    # Without a pinned backend the receiver diagnostic is an exit 3 refusal,
    # but still prints a deterministic JSON snapshot classification.
    cfg4 = tmp / "receiver.conf"
    cfg4.write_text(IDENTITY)
    good["carplay_psk"] = "test-passphrase"
    good["updated_ms"] = int(time.time()) * 1000
    empty_backend = tmp / "empty-backend"
    empty_backend.mkdir()
    for snap_text, snap_name in ((json.dumps(good), "fresh"),
                                 (json.dumps(good | dict(
                                     updated_ms=good["updated_ms"] - 6000)),
                                  "stale")):
        snapshot.write_text(snap_text)
        r = subprocess.run([str(BROKER), "--check-receiver", "--config",
                            str(cfg4), "--status", str(snapshot),
                            "--receiver-interface", "lo",
                            "--receiver-timeout-ms", "300",
                            "--receiver-settle-ms", "50",
                            "--receiver-backend-root", str(empty_backend)],
                           capture_output=True, text=True, timeout=10)
        assert r.returncode == 3, r
        payload = json.loads(r.stdout)
        assert payload["snapshot"] == snap_name, payload
        assert payload["receiver"] == "backend-pin", payload
        assert payload["ok"] is False, payload
    r = subprocess.run([str(BROKER), "--check-receiver", "--config",
                        str(cfg4), "--status", str(tmp / "no-snapshot"),
                        "--receiver-interface", "lo",
                        "--receiver-timeout-ms", "300",
                        "--receiver-settle-ms", "50",
                        "--receiver-backend-root", str(empty_backend)],
                       capture_output=True, text=True, timeout=10)
    assert r.returncode == 3, r
    payload = json.loads(r.stdout)
    assert payload["snapshot"] == "missing", payload

    # An explicitly allowed but absent diagnostic library is no-backend.
    r = subprocess.run([str(BROKER), "--check-receiver", "--config",
                        str(cfg4), "--status", str(snapshot),
                        "--receiver-interface", "lo",
                        "--receiver-timeout-ms", "300",
                        "--receiver-settle-ms", "50",
                        "--dns-sd-library", str(tmp / "absent.so"),
                        "--allow-unpinned-dns-sd"],
                       capture_output=True, text=True, timeout=10)
    assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
    assert json.loads(r.stdout)["receiver"] == "no-backend"

    # A complete config must fail at the receiver gate, before the RX socket
    # is created.  No /run/wifi1b path is touched.
    cfg3 = tmp / "complete.conf"
    cfg3.write_text(IDENTITY + "receiver_timeout_ms=300\n")
    status3 = tmp / "s3"
    r = subprocess.run([str(BROKER), "--config", str(cfg3),
                        "--broker-status", str(status3),
                        "--status", str(tmp / "no-daemon-status"),
                        "--usb-sysfs", str(usb_empty)],
                       capture_output=True, text=True, timeout=20)
    assert r.returncode == 2, r
    text = status3.read_text()
    assert '"state":"receiver_incomplete"' in text, text
    assert '"ok":false' in text, text
    assert "AP snapshot not ready" in text, text
    assert "cannot create rx" not in text, text

    # --- mock DNS-SD success path -------------------------------------------
    mock = tmp / "mock-dns-sd.so"
    mock_source = tmp / "mock_dns_sd.c"
    mock_source.write_text(MOCK_SOURCE)
    build_mock(mock, mock_source)

    comm = Path("/proc/self/comm").read_text().strip()
    wildcard = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    wildcard.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    wildcard.bind(("0.0.0.0", 0))
    wildcard.listen(1)
    port = wildcard.getsockname()[1]

    cfg5 = tmp / "receiver-mock.conf"
    cfg5.write_text(IDENTITY + f"receiver_listener_owner={comm}\n")
    snapshot.write_text(json.dumps(good))
    pi = "policy-test-pi-0123456789abcdef"
    r = run_check(BROKER, cfg5, snapshot, port, {}, mock)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["ok"] is True, payload
    assert payload["receiver"] == "ok", payload
    assert payload["receiver_port"] == port, payload
    assert payload["receiver_service_count"] == 1, payload
    assert payload["receiver_source_version"] == "320.17.6", payload
    assert payload["receiver_pi_sha256"] == hashlib.sha256(
        pi.encode()).hexdigest(), payload
    assert payload["receiver_pi_len"] == len(pi), payload
    assert payload["listener_scope"] == "wildcard", payload
    assert payload["listener_owner"] == comm, payload
    assert payload["listener_owner_allowed"] is True, payload
    assert payload["snapshot"] == "fresh", payload
    assert pi not in r.stdout + r.stderr
    assert "test-passphrase" not in r.stdout + r.stderr

    # Stale and missing snapshots are reported, not treated as blockers.
    snapshot.write_text(json.dumps(
        good | dict(updated_ms=good["updated_ms"] - 6000)))
    r = run_check(BROKER, cfg5, snapshot, port, {}, mock)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert json.loads(r.stdout)["snapshot"] == "stale"
    r = run_check(BROKER, cfg5, tmp / "absent-snapshot", port, {}, mock)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert json.loads(r.stdout)["snapshot"] == "missing"
    snapshot.write_text(json.dumps(good))

    # Observed service names are JSON-escaped.
    weird = 'we"ird\\name'
    r = run_check(BROKER, cfg5, snapshot, port, {}, mock, name=weird)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver_service_name"] == weird, payload
    assert '"receiver_service_name":"we\\"ird\\\\name"' in r.stdout, r.stdout

    # Owner allowlist failure is reported but not accepted.  The config for
    # this run omits the key so the CLI override is not overwritten.
    cfg6 = tmp / "receiver-owner.conf"
    cfg6.write_text(IDENTITY + "receiver_timeout_ms=300\n")
    r = run_check(BROKER, cfg6, snapshot, port, {}, mock,
                  listener_owner="SomeOtherProcess")
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver"] == "listener-owner", payload
    assert payload["listener_owner"] == comm, payload
    assert payload["listener_owner_allowed"] is False, payload

    # Duplicate Add for one instance is accepted, not ambiguous.
    r = run_check(BROKER, cfg5, snapshot, port,
                  {"CP_MOCK_DUPLICATE_ADD": "1"}, mock)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    assert json.loads(r.stdout)["receiver_service_count"] == 1

    # Two distinct valid advertisements are ambiguous.
    r = run_check(BROKER, cfg5, snapshot, port, {"CP_MOCK_SERVICES": "2"},
              mock)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver"] == "ambiguous", payload

    # One invalid TXT and one valid service: accept the valid one.
    r = run_check(BROKER, cfg5, snapshot, port,
                  {"CP_MOCK_SERVICES": "2", "CP_MOCK_INVALID_FIRST": "1"},
                  mock)
    assert r.returncode == 0, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver"] == "ok", payload
    assert payload["receiver_rejected_count"] == 1, payload

    # No advertisement within the bounded window.
    r = run_check(BROKER, cfg5, snapshot, port, {"CP_MOCK_NONE": "1"},
              mock)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    assert json.loads(r.stdout)["receiver"] == "no-service"

    # Loopback-only listener is a scope refusal.
    loopback = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    loopback.bind(("127.0.0.1", 0))
    loopback.listen(1)
    loop_port = loopback.getsockname()[1]
    r = run_check(BROKER, cfg5, snapshot, loop_port, {}, mock)
    assert r.returncode == 2, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver"] == "listener-scope", payload
    assert payload["listener_scope"] == "loopback", payload

    # Pinned libdns_sd mismatch fails closed before dlopen.
    pin_root = tmp / "pin-root"
    (pin_root / "app" / "lib").mkdir(parents=True)
    (pin_root / "app" / "lib" / "libdns_sd.so").write_bytes(b"not-pinned")
    r = subprocess.run(
        [str(BROKER), "--check-receiver", "--config", str(cfg5),
         "--status", str(snapshot), "--receiver-interface", "lo",
         "--receiver-timeout-ms", "300", "--receiver-settle-ms", "50",
         "--receiver-backend-root", str(pin_root)],
        capture_output=True, text=True, timeout=10)
    assert r.returncode == 3, (r.returncode, r.stdout, r.stderr)
    payload = json.loads(r.stdout)
    assert payload["receiver"] == "backend-pin", payload

    wildcard.close()
    loopback.close()

print("PASS: broker helper config/identity/security/receiver gates")
