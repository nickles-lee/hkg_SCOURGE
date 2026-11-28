/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * AA interceptor PROTOTYPE — LD_PRELOAD shim for AppProjection.
 *
 * Interposes the three exported NavigationStatusCallback virtuals in
 * liblinux-head-unit.so by vtable-entry override (docs/hudnav/AA_PROTOCOL.md
 * §1: GLOBAL DEFAULT symbols, named R_X86_64_64 vtable relocs, no DF_SYMBOLIC).
 * Calls the original FIRST (dlsym RTLD_NEXT), then forwards a normalized record to
 * hudnavd over a unix socket. FAIL-OPEN: any error still returns the original result;
 * never takes ownership of / frees the std::string args.
 *
 * PROVISIONAL: the next-turn argument order is inferred from log strings
 * (road, turnSide, turnAngle, image, turnNumber, …); verify in VM before car use.
 *
 * ACTIVATION (docs/hudnav/AA_PROTOCOL.md): the shim is appended to the
 * launcher-wide LD_PRELOAD in /app/bin/AppLauncher.sh, so it is *mapped* into
 * every AppLauncher child. A process self-gate keeps it functionally dormant
 * everywhere except AppProjection: no socket, no records, originals still run.
 * HUDNAV_FORCE=1 overrides the gate for host tests only.
 *
 * Build: gcc -shared -fPIC -o out/aa_shim.so src/aa_shim.c src/convert.c   (no libdl: see shim_lookup)
 */
#define _GNU_SOURCE
#include "hudnav.h"
#include <link.h>
#include <elf.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
/* Mangled names to look up come from the build-time firmware profile
 * (tools/profile_to_header.py). The exported interposer function names below
 * stay literal: they are this shim's own ABI, not profile data. */
#include "fw_profile.h"

/* NO dlsym in this library, deliberately (VM rehearsal 2026-11-25, docs/hudnav/AA_PROTOCOL.md).
 * AppProjection loads no libdl, so on the target's glibc 2.25 (dlsym lives in libdl.so.2) any dlsym
 * reference is unresolvable there: with eager binding ld.so aborts the process at startup, and with lazy
 * binding the first call dies with "symbol lookup error" -- which would kill AppProjection the moment
 * navigation events (or our interposers) first run. Instead we look symbols up ourselves with
 * dl_iterate_phdr (public libc, no libdl) and a walk of each object's .dynsym. Works identically on the
 * host (so the host tests exercise the shipped code) and on the target. */
typedef struct {
    const char *name;
    int         use_next;     /* 1 = RTLD_NEXT semantics (only objects loaded AFTER this library) */
    uintptr_t   self;         /* an address inside this library */
    int         past_self;
    void       *found;
    uintptr_t   found_base;
    const char *found_obj;
} find_ctx;

static uintptr_t dyn_addr(uintptr_t base, uintptr_t v) { return v < base ? v + base : v; }

static size_t gnu_hash_symcount(const uint32_t *h)
{
    uint32_t nb = h[0], symoff = h[1], bloom = h[2];
    const uint64_t *bl = (const uint64_t *)(h + 4);
    const uint32_t *buckets = (const uint32_t *)(bl + bloom);
    const uint32_t *chain = buckets + nb;
    uint32_t last = 0;
    for (uint32_t i = 0; i < nb; i++) if (buckets[i] > last) last = buckets[i];
    if (last < symoff) return symoff;
    while (!(chain[last - symoff] & 1u)) last++;
    return (size_t)last + 1;
}

static int find_cb(struct dl_phdr_info *info, size_t size, void *arg)
{
    find_ctx *c = (find_ctx *)arg; (void)size;
    const ElfW(Dyn) *dyn = NULL;
    uintptr_t lo = (uintptr_t)-1, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            uintptr_t a = info->dlpi_addr + ph->p_vaddr, b = a + ph->p_memsz;
            if (a < lo) lo = a;
            if (b > hi) hi = b;
        } else if (ph->p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + ph->p_vaddr);
        }
    }
    if (c->self >= lo && c->self < hi) { c->past_self = 1; return 0; }
    if (c->use_next && !c->past_self) return 0;
    if (!dyn) return 0;
    const ElfW(Sym) *symtab = NULL; const char *strtab = NULL;
    const uint32_t *hash = NULL, *gnuhash = NULL;
    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
        case DT_SYMTAB:   symtab  = (const ElfW(Sym) *)dyn_addr(info->dlpi_addr, d->d_un.d_ptr); break;
        case DT_STRTAB:   strtab  = (const char *)dyn_addr(info->dlpi_addr, d->d_un.d_ptr); break;
        case DT_HASH:     hash    = (const uint32_t *)dyn_addr(info->dlpi_addr, d->d_un.d_ptr); break;
        case DT_GNU_HASH: gnuhash = (const uint32_t *)dyn_addr(info->dlpi_addr, d->d_un.d_ptr); break;
        default: break;
        }
    }
    if (!symtab || !strtab || (!hash && !gnuhash)) return 0;
    size_t n = hash ? (size_t)hash[1] : gnu_hash_symcount(gnuhash);
    for (size_t i = 1; i < n; i++) {
        const ElfW(Sym) *s = &symtab[i];
        if (s->st_shndx == SHN_UNDEF || s->st_value == 0) continue;
        int t = ELF64_ST_TYPE(s->st_info);
        if (t != STT_FUNC && t != STT_OBJECT) continue;
        if (strcmp(strtab + s->st_name, c->name) != 0) continue;
        c->found = (void *)(info->dlpi_addr + s->st_value);
        c->found_base = info->dlpi_addr;
        c->found_obj = info->dlpi_name;
        return 1;
    }
    return 0;
}

static void *shim_lookup(const char *name, int use_next, uintptr_t *base, const char **obj)
{
    find_ctx c; memset(&c, 0, sizeof c);
    c.name = name; c.use_next = use_next; c.self = (uintptr_t)&shim_lookup;
    dl_iterate_phdr(find_cb, &c);
    if (base) *base = c.found_base;
    if (obj) *obj = c.found_obj;
    return c.found;
}
static void *shim_next(const char *name)    { return shim_lookup(name, 1, NULL, NULL); }
static void *shim_default(const char *name) { return shim_lookup(name, 0, NULL, NULL); }

/* std::string const& is passed as a pointer to the libstdc++ string object;
 * its data pointer is at offset 0 (SSO layout: _M_p). We only read it for the
 * road name (optional; v1 can ignore). Do NOT free or modify it. */
typedef void (*status_fn)(void *self, int status);
typedef void (*dist_fn)(void *self, int meters, int time_s, int disp_e3, int unit);
typedef void (*turn_fn)(void *self, const void *road, int a1, int a2,
                        const void *image, int a3, int a4);

static int       sock = -1;
static unsigned char send_busy;

/* Process self-gate. The shim is preloaded launcher-wide, so it must be inert
 * outside AppProjection. Resolved once, lazily, from /proc/self/exe (the
 * unambiguous signal) with APPID=110 as a fallback. Never causes a syscall
 * failure to abort the host app: an unreadable /proc leaves it inert. */
static int g_active = -1;

static int shim_active(void)
{
    if (g_active >= 0) return g_active;
    int on = 0;
    const char *force = getenv("HUDNAV_FORCE");
    if (force && force[0] == '1' && force[1] == '\0') on = 1;
    if (!on) {
        char exe[256];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = '\0';
            const char *base = strrchr(exe, '/');
            base = base ? base + 1 : exe;
            if (strcmp(base, "AppProjection") == 0) on = 1;
        }
    }
    if (!on) {
        const char *appid = getenv("APPID");
        if (appid && strcmp(appid, "110") == 0) on = 1;
    }
    g_active = on;
    return on;
}

static void ensure_sock(void)
{
    if (sock >= 0) return;
    const char *path = getenv("HUDNAV_SOCK");
    if (!path) path = "/tmp/hudnav.sock";
    sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0) return;
    struct sockaddr_un a; memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path, sizeof a.sun_path - 1);
    if (connect(sock, (struct sockaddr *)&a, sizeof a) < 0) { close(sock); sock = -1; }
}
static void sendline(const char *s)
{
    if (__atomic_test_and_set(&send_busy, __ATOMIC_ACQUIRE))
        return;
    ensure_sock();
    if (sock >= 0) {
        size_t n = strlen(s);
        if (send(sock, s, n, MSG_NOSIGNAL) != (ssize_t)n) {
            close(sock);
            sock = -1;
        }
    }
    __atomic_clear(&send_busy, __ATOMIC_RELEASE);
}

static void aa_trace(const char *msg);

/* exact mangled names (projection_hookpoints.md §1.1) */
void _ZN24NavigationStatusCallback24navigationStatusCallbackEi(void *self, int status)
{
    status_fn real_status = (status_fn)shim_next(HUDNAV_AA_SYM_STATUS_CB);
    if (real_status) real_status(self, status);   /* original first */
    if (!shim_active()) return;
    aa_trace("CB navigationStatus");
    /* 0 Unavailable, 1 Active, 2 Inactive -> hn_navstate 0/1/0 */
    char buf[64];
    snprintf(buf, sizeof buf, "STATE s=%d raw=%d\nHB\n", status == 1 ? 1 : 0, status);
    sendline(buf);
}

void _ZN24NavigationStatusCallback34navigationNextTurnDistanceCallbackEiii45NavigationNextTurnDistanceEvent_DistanceUnits(
    void *self, int meters, int time_s, int disp_e3, int unit)
{
    dist_fn real_dist = (dist_fn)shim_next(HUDNAV_AA_SYM_DISTANCE_CB);
    if (real_dist) real_dist(self, meters, time_s, disp_e3, unit);
    if (!shim_active()) return;
    aa_trace("CB navigationNextTurnDistance");
    char buf[120];
    snprintf(buf, sizeof buf, "DIST m=%d disp=%d unit=%d time=%d\nHB\n", meters, disp_e3, unit, time_s);
    sendline(buf);
}

void _ZN24NavigationStatusCallback26navigationNextTurnCallbackERKNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEiiS7_ii(
    void *self, const void *road, int a1, int a2, const void *image, int a3, int a4)
{
    turn_fn real_turn = (turn_fn)shim_next(HUDNAV_AA_SYM_TURN_CB);
    if (real_turn) real_turn(self, road, a1, a2, image, a3, a4);
    if (!shim_active()) return;
    aa_trace("CB navigationNextTurn");
    /* PROVISIONAL arg mapping: a1=turnSide(1L/2R), a2=NextTurnEnum event,
     * a3=turnAngle, a4=turnNumber. Verify in VM. */
    int side = a1, event = a2, angle = a3;
    hn_maneuver man = hn_normalize_aa(event, side, angle);
    char buf[160];
    snprintf(buf, sizeof buf, "TURN src=aa man=%d exit=%d raw=%d side=%d fourth=%d\nHB\n",
             (int)man, angle, event, side, a4);
    sendline(buf);
}

/* ======================================================================================
 * AA navigation ENABLEMENT (offline RE 2026-11-25, research/offline_re/T1,T2,T5,T6, reviewer-verified)
 *
 * On this firmware the head unit never declares the Android Auto NavigationStatusService, so the
 * phone never sends turn data and the three callbacks above never fire. Two things are missing:
 *  1. CAndroidAutoMain::setInstanceINavigationStatusServiceCB(non-null) is never called by AppProjection
 *     (initOptionalServices registers the endpoint only when that flag is set). AppProjection calls the
 *     other four setters, in CAAPCtrlCB::onCreateAAPInterface, with the interface pointer as first arg.
 *     We interpose the LAST of them (PhoneStatus) and add the navigation setter on the same interface.
 *     The stored callback is later invoked by CEventThread::handleOptionCallbackEvent through vtable slots
 *     0x10/0x18/0x20 only, so a static object with an all-no-op vtable is a safe stand-in.
 *  2. NavigationStatusEndpoint::start() (libreceiver-lib.so, sends NavigationStatusStart 0x8001) is never
 *     called by anything. We interpose the inherited ProtocolEndpointBase::onChannelOpened (a named vtable
 *     reloc; it runs AFTER the ChannelOpenResponse) and call start() when `this` is the navigation endpoint.
 *
 * SAFETY: both are inert unless the file /rw_data/hudnav/aa-nav-enable exists AND the process is
 * AppProjection. Every failure path returns having already called the original (fail-open).
 * Optional trace: if /rw_data/hudnav/aa-trace exists, one line per event goes to /tmp/hudnav-aa-trace.log.
 * ====================================================================================== */
#include <fcntl.h>

#define NAV_ENABLE_FILE "/rw_data/hudnav/aa-nav-enable"
#define NAV_TRACE_FILE  "/rw_data/hudnav/aa-trace"
#define NAV_TRACE_LOG   "/tmp/hudnav-aa-trace.log"

/* Host-test overrides (never set on the car). */
static const char *nav_path(const char *env, const char *dflt)
{
    const char *v = getenv(env);
    return (v && v[0]) ? v : dflt;
}

static void aa_trace(const char *msg)
{
    if (access(nav_path("HUDNAV_AA_TRACE_FILE", NAV_TRACE_FILE), F_OK) != 0) return;
    int fd = open(nav_path("HUDNAV_AA_TRACE_LOG", NAV_TRACE_LOG), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    ssize_t w = write(fd, msg, strlen(msg)); (void)w;
    w = write(fd, "\n", 1); (void)w;
    close(fd);
}

static int nav_enabled(void)
{
    return shim_active() && access(nav_path("HUDNAV_AA_NAV_ENABLE_FILE", NAV_ENABLE_FILE), F_OK) == 0;
}

/* Stand-in for INavigationStatusServiceCB: vptr -> table of no-op functions (slots 0x00..0x78). */
static void nav_noop(void) { }
static void *g_nav_vtbl[16];
static void *g_nav_obj;

static void *nav_dummy_cb(void)
{
    if (!g_nav_obj) {
        for (int i = 0; i < 16; i++) g_nav_vtbl[i] = (void *)nav_noop;
        g_nav_obj = (void *)g_nav_vtbl;
    }
    return (void *)&g_nav_obj;
}

typedef void (*set_cb_fn)(void *iface, void *cb);
typedef void (*chan_fn)(void *self, unsigned char channel);
typedef void (*start_fn)(void *self);

void _ZN21CAndroidAutoInterface32setInstanceIPhoneStatusServiceCBEP21IPhoneStatusServiceCB(void *iface, void *cb)
{
    set_cb_fn real = (set_cb_fn)shim_next(HUDNAV_AA_SYM_PHONE_SETTER);
    if (real) real(iface, cb);                          /* original first */
    /* No once-only flag: AppProjection may rebuild its CAndroidAutoInterface per phone connection, and the
     * navigation setter is idempotent (sets a flag and stores a pointer), so add it every time the
     * callbacks are registered. */
    if (!iface || !nav_enabled()) return;
    set_cb_fn nav = (set_cb_fn)shim_next(HUDNAV_AA_SYM_NAV_SETTER);
    if (!nav) { aa_trace("NAV setter symbol not found"); return; }
    nav(iface, nav_dummy_cb());
    aa_trace("NAV setInstanceINavigationStatusServiceCB called (dummy cb)");
}

void _ZN20ProtocolEndpointBase15onChannelOpenedEh(void *self, unsigned char channel)
{
    chan_fn real = (chan_fn)shim_next(HUDNAV_AA_SYM_ON_CHANNEL_OPENED);
    if (real) real(self, channel);                      /* original first */
    if (!self || !nav_enabled()) return;
    void *vt = shim_default(HUDNAV_AA_SYM_NAV_VTABLE);
    if (!vt || *(void **)self != (void *)((char *)vt + 0x10)) return;   /* not the nav endpoint */
    start_fn start = (start_fn)shim_default(HUDNAV_AA_SYM_NAV_START);
    if (!start) { aa_trace("NAV start symbol not found"); return; }
    start(self);
    aa_trace("NAV NavigationStatusEndpoint::start() called after channel open");
}

/* Self-test (VM / car diagnosis only): with HUDNAV_AA_SELFTEST=1 in the environment, print where every
 * symbol the interposers need resolves to, as `object + offset`, so it can be compared with the pinned
 * firmware (the 260319 expected offsets are recorded in the selected profile under aa.sym.*;
 * the shim itself never hardcodes them). Does nothing otherwise. */
__attribute__((constructor)) static void shim_selftest(void)
{
    const char *e = getenv("HUDNAV_AA_SELFTEST");
    if (!e || e[0] != '1' || e[1] != '\0' || !shim_active()) return;
    static const struct { const char *name; int next; } q[] = {
        { HUDNAV_AA_SYM_PHONE_SETTER,     1 },
        { HUDNAV_AA_SYM_NAV_SETTER,       1 },
        { HUDNAV_AA_SYM_ON_CHANNEL_OPENED, 1 },
        { HUDNAV_AA_SYM_NAV_START,        0 },
        { HUDNAV_AA_SYM_NAV_VTABLE,       0 },
        { HUDNAV_AA_SYM_STATUS_CB,        1 },
    };
    for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++) {
        uintptr_t base = 0; const char *obj = "";
        void *p = shim_lookup(q[i].name, q[i].next, &base, &obj);
        char line[400];
        int n = snprintf(line, sizeof line, "SELFTEST %s %s off=0x%lx obj=%s\n",
                         q[i].name, p ? "FOUND" : "NOT-FOUND",
                         p ? (unsigned long)((uintptr_t)p - base) : 0ul, obj ? obj : "");
        if (n > 0) { ssize_t w = write(2, line, (size_t)n); (void)w; }
    }
}
