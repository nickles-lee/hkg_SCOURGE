/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "../src/cp_rg_rx.h"
#include "../src/rgd_tlv.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int checks, failures;
#define CHECK(c) do { checks++; if (!(c)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static const uint8_t detect[] = { 0xff, 0x55, 0x02, 0x00, 0xee, 0x10 };
static int listener, peer = -1;
static uint8_t seq = 0x8d;   /* The last retained phone data packet is 0x8c. */

static void put16(uint8_t *p, unsigned value)
{
    p[0] = (uint8_t)(value >> 8);
    p[1] = (uint8_t)value;
}

static size_t field(uint8_t *p, int id, const void *bytes, size_t n)
{
    put16(p, n + 4);
    put16(p + 2, id);
    if (n) memcpy(p + 4, bytes, n);
    return n + 4;
}

static size_t integer(uint8_t *p, int id, uint64_t value, unsigned width)
{
    uint8_t bytes[8];
    for (unsigned i = 0; i < width; i++)
        bytes[width - 1 - i] = (uint8_t)(value >> (i * 8));
    return field(p, id, bytes, width);
}

static void message_header(uint8_t *p, size_t size, int id)
{
    p[0] = p[1] = 0x40;
    put16(p + 2, size);
    put16(p + 4, id);
}

static size_t maneuver(uint8_t *p, int index, int type, int angle)
{
    size_t n = 6;
    n += integer(p + n, 0, 42, 2);
    n += integer(p + n, 1, index, 2);
    n += integer(p + n, 3, type, 1);
    n += integer(p + n, 8, 1, 1);  /* LHT */
    n += integer(p + n, 9, type == 29, 1);
    n += integer(p + n, 10, (uint16_t)-45, 2);
    n += integer(p + n, 10, 90, 2);
    n += integer(p + n, 11, (uint16_t)angle, 2);
    message_header(p, n, RGD_MSG_MANEUVER);
    return n;
}

static size_t update(uint8_t *p, int state, int index, int supported)
{
    size_t n = 6;
    n += integer(p + n, 0, 42, 2);
    n += integer(p + n, 1, state, 1);
    n += integer(p + n, 5, UINT64_C(5000000000), 8);
    n += integer(p + n, 6, 660, 8);
    n += integer(p + n, 7, 4300, 4);
    n += integer(p + n, 10, 1200, 4);
    n += field(p + n, 11, "1.2", 4);
    n += integer(p + n, 12, 0, 1); /* iAP2 km */
    if (index >= 0) {
        uint8_t list[4];
        put16(list, index);
        put16(list + 2, index + 1);
        n += field(p + n, 13, list, sizeof list);
    }
    if (supported >= 0) n += integer(p + n, 20, supported, 1);
    message_header(p, n, RGD_MSG_UPDATE);
    return n;
}

static uint8_t checksum(const uint8_t *p, size_t n)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < n; i++) sum -= p[i];
    return sum;
}

static size_t packet(uint8_t *p, int session, const uint8_t *data, size_t n)
{
    size_t len = n ? 10 + n : 9;
    p[0] = 0xff;
    p[1] = 0x5a;
    put16(p + 2, len);
    p[4] = 0x40;
    p[5] = seq;
    p[6] = 0x75;
    p[7] = (uint8_t)session;
    p[8] = checksum(p, 8);
    if (n) {
        seq++;
        memcpy(p + 9, data, n);
        p[9 + n] = checksum(data, n);
    }
    return len;
}

static void feed(int session, const uint8_t *msg, size_t size)
{
    uint8_t p[256];
    cp_rx_feed(p, (long)packet(p, session, msg, size));
}

static int ready(int fd)
{
    struct pollfd p = { .fd = fd, .events = POLLIN };
    return poll(&p, 1, 0) > 0 && (p.revents & POLLIN);
}

#define expect_silence() expect_silence_(__LINE__)
static void expect_silence_(int ln)
{
    int r = ready(peer >= 0 ? peer : listener);
    CHECK(!r);
    if (r && peer >= 0) {
        char b[512]; ssize_t c = recv(peer, b, sizeof b - 1, MSG_DONTWAIT);
        if (c > 0) { b[c] = 0; fprintf(stderr, "line %d unexpected <%s>\n", ln, b); }
    }
}

static void expect_lines(const char *expected)
{
    if (peer < 0 && ready(listener)) peer = accept(listener, NULL, NULL);
    char line[512] = "";
    size_t used = 0;
    while (peer >= 0 && used < sizeof line - 1) {
        ssize_t count = recv(peer, line + used, sizeof line - used - 1, MSG_DONTWAIT);
        if (count <= 0) break;
        used += (size_t)count;
    }
    line[used] = 0;
    CHECK(strcmp(line, expected) == 0);
    if (strcmp(line, expected))
        fprintf(stderr, "expected <%s>, got <%s>\n", expected, line);
}

static void replay(const char *path, size_t chunk)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        /* Optional redacted car-capture fixture. It is deliberately NOT shipped:
         * captures can contain private data. A porter may place their own
         * sanitized capture here; without one the test skips this input only. */
        fprintf(stderr, "SKIP: optional fixture %s not present (not shipped)\n", path);
        return;
    }
    uint8_t b[8192];
    size_t n, total = 0;
    while ((n = fread(b, 1, chunk, f))) {
        cp_rx_feed(b, (long)n);
        total += n;
    }
    CHECK(!ferror(f) && total > 100);
    fclose(f);
    expect_silence();
}

static void expect_turn(int man, int angle, int raw, int ordinal)
{
    char lines[240];
    snprintf(lines, sizeof lines,
             "STATE s=1\nTURN src=cp man=%d exit=%d raw=%d side=1 fourth=%d\n"
             "DIST m=1200 disp=1200 unit=1\nDEST m=4300\nHB\n",
             man, angle, raw, ordinal);
    expect_lines(lines);
}

static void test_real_then_guidance(void)
{
    /* Optional redacted car-capture fixtures are deliberately NOT shipped. This
     * scenario uses them to establish a pre-guidance state, so skip it as a whole
     * when they are absent rather than reporting false failures. */
    FILE *probe = fopen("tests/fixtures/cp_capture_write.bin", "rb");
    if (!probe) {
        fprintf(stderr, "SKIP: test_real_then_guidance (optional capture fixtures not shipped)\n");
        return;
    }
    fclose(probe);
    replay("tests/fixtures/cp_capture_write.bin", 8192);
    replay("tests/fixtures/cp_capture_read.bin", 1);

    uint8_t m[80], u[120], p[256];
    size_t nm = maneuver(m, 7, 2, 90);
    size_t nu = update(u, 1, 7, 1);

    /* The file-transfer session can carry even a valid-looking 4040 message. */
    feed(2, u, nu);
    feed(2, m, nm);
    expect_silence();

    /* Embedded bad FF5A header, partial FF5A header, coalesced ACK + data,
     * a control sync split across packets, and a duplicate fragment. */
    static const uint8_t junk[] = { 0x40, 0x40, 0xff, 0x5a, 0xff, 0xff,
                                    0x40, 0x28, 0x75, 1, 0, 0xff };
    cp_rx_feed(junk, sizeof junk);
    size_t a = packet(p, 1, m, 1);
    errno = EDOM;
    cp_rx_feed(p, 3);
    cp_rx_feed(p + 3, (long)a - 3);
    CHECK(errno == EDOM);
    cp_rx_feed(p, (long)a);
    a = packet(p, 0, NULL, 0);
    a += packet(p + a, 1, m + 1, nm - 1);
    a += packet(p + a, 1, u, nu);
    cp_rx_feed(p, (long)a);
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);

    /* A pre-sent turn at the next index must not replace the current turn. */
    feed(1, m, maneuver(m, 8, 29, -90));
    expect_silence();
    feed(1, u, update(u, 1, 8, -1));
    expect_turn(HN_MAN_ROUNDABOUT, 270, 29, 2);

    /* Bad payload and a sequence gap invalidate an in-flight AP2 message. */
    feed(1, u, 18);
    a = packet(p, 1, u + 18, nu - 18);
    p[a - 1] ^= 1;
    cp_rx_feed(p, (long)a);
    expect_silence();
    feed(1, u, 18);
    seq++;
    feed(1, u + 18, nu - 18);
    expect_silence();

    /* A fresh cursor can arrive before its indexed descriptor. */
    feed(1, u, update(u, 1, 9, 1));
    expect_lines("STATE s=0\n");
    a = maneuver(m, 9, 1, -90);
    feed(1, m, a);
    expect_turn(HN_MAN_TURN_LEFT, -90, 1, -1);

    feed(1, u, update(u, 5, -1, -1));
    expect_lines("STATE s=0\nSTATE s=2\n");
    feed(1, m, maneuver(m, 0, 2, 90));
    feed(1, u, update(u, 1, 0, -1));
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);
    feed(1, u, update(u, 0, -1, -1));
    expect_lines("STATE s=0\n");
}

static void test_interleaved(void)
{
    /* No SYN yet: fragments are keyed by the session ID. */
    cp_rx_feed(detect, sizeof detect);
    uint8_t m[80], u[120], p[256];
    size_t nm = maneuver(m, 0, 2, 90), nu = update(u, 1, 0, 1);
    seq = 254;
    feed(3, m, 14);
    feed(4, u, 17);
    size_t n = packet(p, 3, m + 14, nm - 14);
    n += packet(p + n, 4, u + 17, nu - 17);
    cp_rx_feed(p, (long)n);
    expect_silence();
    feed(3, u, nu);
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);

    /* Two control messages concatenated in a single link payload. */
    uint8_t pair[200];
    nm = maneuver(pair, 1, 2, 90);
    nu = update(pair + nm, 1, 1, -1);
    feed(4, pair, nm + nu);
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);
    cp_rx_feed(detect, sizeof detect);
    expect_lines("STATE s=0\nSTATE s=0\n");
    /* Car 2026-11-27: a real iPhone never sends TLV 0x14, so a fresh session with no 0x14 publishes. */
    feed(4, pair, nm + nu);
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);
}

static void test_fields(void)
{
    uint8_t b[160], group[40];
    hn_rgd_state u;
    size_t n = update(b, 1, 7, 1);
    CHECK(hn_rgd_parse_update(b, n, &u));
    CHECK(u.eta_seconds == UINT64_C(5000000000) && u.time_remaining_s == 660);
    CHECK(u.dist_maneuver_m == 1200 && u.dist_display_e3 == 1200 && u.dist_display_unit == 0);
    CHECK(u.next_index == 7 && u.dist_dest_m == 4300);
    b[n] = 0;
    message_header(b, n + 1, RGD_MSG_UPDATE);
    CHECK(!hn_rgd_parse_update(b, n + 1, &u));

    hn_rgd_maneuver m;
    n = maneuver(b, 3, 29, -1);
    CHECK(hn_rgd_parse_maneuver(b, n, &m));
    CHECK(m.junction_type == 1 && m.junction_angle_count == 2 &&
          m.junction_angles[0] == -45 && m.junction_angles[1] == 90);
    CHECK(m.exit_angle == -1 && hn_rgd_to_turn(&m).exit_angle == 359);
    m.present &= ~(1u << 11);
    CHECK(hn_rgd_to_turn(&m).exit_angle == -1);  /* no spoke-based guess */
    n = maneuver(b, 3, 29, 0);
    CHECK(hn_rgd_parse_maneuver(b, n, &m) && hn_rgd_to_turn(&m).exit_angle == 0);

    size_t g = integer(group, 0, 2, 2);
    g += integer(group + g, 1, 2, 1);
    g += integer(group + g, 2, (uint16_t)-45, 2);
    g += integer(group + g, 3, 0, 2);
    n = 6;
    n += integer(b + n, 0, 42, 2);
    n += integer(b + n, 1, 123, 2);
    n += field(b + n, 2, group, g);
    n += field(b + n, 3, "Use left lane", 14);
    message_header(b, n, RGD_MSG_LANES);
    hn_rgd_lanes lanes;
    CHECK(hn_rgd_parse_lanes(b, n, &lanes));
    CHECK(lanes.index == 123 && lanes.lane_count == 1);
    CHECK(lanes.lanes[0].position == 2 && lanes.lanes[0].status == 2 &&
          lanes.lanes[0].angles[0] == -45 && lanes.lanes[0].highlight == 0);
    CHECK(strcmp(lanes.description, "Use left lane") == 0);
    feed(1, b, n);
    expect_silence();
    /* The outer TLV is intact, but the first nested length exceeds its group. */
    b[22] = 0xff;
    CHECK(!hn_rgd_parse_lanes(b, n, &lanes));
}

static void test_maximum_packet(void)
{
    const size_t payload_size = UINT16_MAX - 10;
    uint8_t *msg = calloc(UINT16_MAX, 1);
    uint8_t *p = malloc(UINT16_MAX);
    CHECK(msg && p);
    if (msg && p) {
        message_header(msg, UINT16_MAX, 0x1234);
        size_t n = packet(p, 1, msg, payload_size);
        CHECK(n == UINT16_MAX);
        cp_rx_feed(p, 1024);
        cp_rx_feed(p + 1024, (long)n - 1024);
        feed(1, msg + payload_size, UINT16_MAX - payload_size);
        expect_silence();

        size_t nm = maneuver(msg, 0, 2, 90);
        size_t nu = update(msg + nm, 1, 0, 1);
        feed(1, msg, nm + nu);
        expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);
        feed(1, msg, update(msg, 1, 0, 0));
        expect_lines("STATE s=0\n");
    }
    free(p);
    free(msg);
}

static void test_message_entry(void)
{
    /* Whole messages straight from the stock dispatcher (session 9, no link layer): same output as the link path. */
    uint8_t m[80], u[120];
    size_t nm = maneuver(m, 11, 2, 90);
    size_t nu = update(u, 1, 11, 1);
    if (peer < 0 && ready(listener)) peer = accept(listener, NULL, NULL);
    while (peer >= 0) {                               /* drop output left over from earlier tests */
        char junk[256];
        if (recv(peer, junk, sizeof junk, MSG_DONTWAIT) <= 0) break;
    }
    cp_rx_feed_message(9, u, (long)nu - 1);          /* truncated message: ignored */
    cp_rx_feed_message(9, m, 3);                      /* too short */
    cp_rx_feed_message(9, NULL, 50);
    expect_silence();
    cp_rx_feed_message(9, m, (long)nm);
    cp_rx_feed_message(9, u, (long)nu);
    expect_turn(HN_MAN_TURN_RIGHT, 90, 2, -1);
    /* trailing bytes beyond the message length are ignored: handled as the same single message */
    cp_rx_feed_message(9, u, (long)nu + 5);
    CHECK(ready(peer >= 0 ? peer : listener));
    while (peer >= 0) {
        char junk[256];
        if (recv(peer, junk, sizeof junk, MSG_DONTWAIT) <= 0) break;
    }
}

int main(void)
{
    char dir[] = "/tmp/hudnav-cp-rx-XXXXXX";
    if (!mkdtemp(dir)) return 1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s/rx.sock", dir);
    listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof addr) ||
        listen(listener, 1) || setenv("HUDNAV_SOCK", addr.sun_path, 1)) return 1;

    test_real_then_guidance();
    test_interleaved();
    test_fields();
    test_maximum_packet();
    test_message_entry();
    cp_rx_feed(NULL, 42);
    cp_rx_feed(detect, -1);
    expect_silence();

    if (peer >= 0) close(peer);
    close(listener);
    unlink(addr.sun_path);
    rmdir(dir);
    printf("CP receive: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
