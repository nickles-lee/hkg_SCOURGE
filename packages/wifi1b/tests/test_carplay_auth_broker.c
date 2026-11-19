/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Synthetic phone and private helper fixtures; never OEM/chip authentication. */
#define _GNU_SOURCE
#include "cp_auth_broker.h"
#define main original_session_tests
#include "test_carplay_session.c"
#undef main
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static void setup_broker(cp_auth_broker *broker, const char *path)
{
    cp_oem_auth_async_config config = {0};
    g_now = 1000u;
    memset(&g_auth, 0, sizeof(g_auth));
    memset(&g_policy, 0, sizeof(g_policy));
    g_policy.consent = CP_BOOT_CONSENT_GRANTED;
    phone_init(&g_phone, 32u, 4u, true, false, false);
    g_phone.sent_challenge = true; /* broker test queues a chip-sized digest */
    session_setup(&g_session, 64u, 8u);
    clear_events();
    config.experimental = true;
    config.socket_path = path;
    config.expected_peer_uid = (uint32_t)geteuid();
    CHECK(cp_auth_broker_init(broker, &g_session, &config) == CP_SESSION_OK);
}

int main(void)
{
    char directory[] = "/tmp/opencode/cp-broker-XXXXXX";
    char path[108];
    struct sockaddr_un address = {0};
    cp_auth_broker broker;
    uint8_t incoming[CP_OEM_AUTH_RPC_REQUEST_LEN + CP_OEM_AUTH_RPC_MAX_REQUEST_PAYLOAD];
    uint8_t outgoing[CP_OEM_AUTH_RPC_RESPONSE_LEN + 300];
    cp_oem_auth_rpc_request request = {0};
    cp_oem_auth_rpc_response response;
    int client = -1, delay = 0, operations = 0;
    bool read_request = false, sent_challenge = false;
    bool saw_cert_state = false, saw_response_state = false;
    CHECK(original_session_tests() == 0);
    CHECK(mkdtemp(directory) != NULL);
    CHECK(snprintf(path, sizeof(path), "%s/helper.sock", directory) > 0);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    CHECK(listener >= 0);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    CHECK(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
    CHECK(chmod(path, 0600) == 0 && listen(listener, 4) == 0);
    setup_broker(&broker, path);
    for (unsigned i = 0; i < 5000 && !pred_auth_done(); i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        cp_session_error_t step = cp_auth_broker_service(&broker, 0, g_now);
        if (step != CP_SESSION_OK) fprintf(stderr, "broker=%d helper=%d op=%d pending=%d now=%llu\n", step, broker.helper.completion.error, broker.helper.op, broker.pending, (unsigned long long)g_now);
        CHECK(step == CP_SESSION_OK);
        if (cp_session_auth_state(&g_session) == CP_BOOT_AUTH_CERT_SENT ||
            cp_session_auth_state(&g_session) == CP_BOOT_AUTH_RESPONSE_SENT) {
            CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATING);
            CHECK(cp_session_tick(&g_session, g_now) == CP_SESSION_OK);
            CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATING);
            saw_cert_state |= cp_session_auth_state(&g_session) == CP_BOOT_AUTH_CERT_SENT;
            saw_response_state |= cp_session_auth_state(&g_session) == CP_BOOT_AUTH_RESPONSE_SENT;
        }
        if (g_phone.got_cert && !sent_challenge) {
            CHECK(phone_queue_hex(&g_phone,
                "4040002aaa0200240000"
                "01010101010101010101010101010101"
                "01010101010101010101010101010101", 7u));
            sent_challenge = true;
        }
        if (client < 0) {
            client = accept4(listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            CHECK(client >= 0 || errno == EAGAIN || errno == EWOULDBLOCK);
        }
        if (client >= 0 && !read_request) {
            ssize_t n = recv(client, incoming, sizeof(incoming), MSG_DONTWAIT);
            if (n >= 0) {
                CHECK((size_t)n >= CP_OEM_AUTH_RPC_REQUEST_LEN);
                CHECK(cp_oem_auth_rpc_request_decode(incoming,
                    CP_OEM_AUTH_RPC_REQUEST_LEN, &request));
                CHECK((size_t)n == CP_OEM_AUTH_RPC_REQUEST_LEN + request.payload_len);
                read_request = true;
                delay = 30; /* advance ACK/retransmit clocks while helper waits */
            } else CHECK(errno == EAGAIN || errno == EWOULDBLOCK);
        }
        if (read_request && --delay == 0) {
            memset(&response, 0, sizeof(response));
            response.magic = CP_OEM_AUTH_RPC_MAGIC;
            response.version = CP_OEM_AUTH_RPC_VERSION;
            response.op = request.op;
            response.request_id = request.request_id;
            response.payload_len = request.op == CP_OEM_AUTH_RPC_OP_CERTIFICATE ? 300u : 128u;
            cp_oem_auth_rpc_response_encode(outgoing, &response);
            memset(outgoing + CP_OEM_AUTH_RPC_RESPONSE_LEN, 0x55, response.payload_len);
            CHECK(send(client, outgoing, CP_OEM_AUTH_RPC_RESPONSE_LEN +
                       response.payload_len, MSG_NOSIGNAL) ==
                  (ssize_t)(CP_OEM_AUTH_RPC_RESPONSE_LEN + response.payload_len));
            close(client); client = -1;
            read_request = false;
            operations++;
        }
    }
    CHECK(pred_auth_done() && operations == 2);
    CHECK(saw_cert_state && saw_response_state);
    CHECK(cp_session_state(&g_session) == CP_SESSION_STATE_AUTHENTICATED);
    CHECK(g_auth.cert_calls == 0 && g_auth.signature_calls == 0);
    CHECK(!cp_session_receiver_active(&g_session));
    cp_auth_broker_destroy(&broker, g_now);
    if (client >= 0) { close(client); client = -1; }

    /* A pending helper cannot block ACKs, survive reset, or authenticate late. */
    setup_broker(&broker, path);
    for (unsigned i = 0; i < 1000 && !broker.pending; i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        CHECK(cp_auth_broker_service(&broker, 0, g_now) == CP_SESSION_OK);
    }
    CHECK(broker.pending);
    uint64_t old_generation = broker.request.generation;
    CHECK(cp_session_cancel(&g_session, g_now) == CP_SESSION_OK);
    (void)cp_auth_broker_service(&broker, 0, g_now);
    CHECK(!broker.pending && cp_oem_auth_async_get_fd(&broker.helper) == -1);
    CHECK(g_session.boot.generation != old_generation);
    CHECK(cp_session_auth_state(&g_session) != CP_BOOT_AUTH_SUCCEEDED);
    cp_auth_broker_destroy(&broker, g_now);

    setup_broker(&broker, path);
    for (unsigned i = 0; i < 1000 && !broker.pending; i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        CHECK(cp_auth_broker_service(&broker, 0, g_now) == CP_SESSION_OK);
    }
    CHECK(broker.pending);
    uint64_t deadline = broker.request.deadline_ms;
    CHECK(cp_auth_broker_next_deadline(&broker) <= deadline);
    for (unsigned i = 0; i < 1200 && g_now < deadline; i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        (void)cp_auth_broker_service(&broker, 0, g_now);
    }
    CHECK(g_now >= deadline);
    CHECK(cp_link_state(&g_session.link) == CP_LINK_STATE_NORMAL);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_FAILED);
    CHECK(!broker.pending && cp_oem_auth_async_get_fd(&broker.helper) == -1);
    cp_auth_broker_destroy(&broker, g_now);

    setup_broker(&broker, path);
    for (unsigned i = 0; i < 1000 && !broker.pending; i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        CHECK(cp_auth_broker_service(&broker, 0, g_now) == CP_SESSION_OK);
    }
    CHECK(broker.pending);
    cp_auth_broker_destroy(&broker, g_now);
    CHECK(cp_session_auth_state(&g_session) == CP_BOOT_AUTH_FAILED);

    /* A busy provider is pending, never authentication success. */
    setup_broker(&broker, path);
    CHECK(cp_oem_auth_async_begin_certificate(&broker.helper, 99u, g_now,
                                              g_now + 5000u) ==
          CP_OEM_AUTH_ASYNC_OK);
    bool saw_request = false;
    for (unsigned i = 0; i < 1000 && !saw_request; i++) {
        cp_boot_auth_request busy_req;
        pump_step(&g_session, &g_phone, 64u, true);
        CHECK(cp_auth_broker_service(&broker, 0, g_now) == CP_SESSION_OK);
        if (cp_session_auth_request(&g_session, &busy_req)) saw_request = true;
    }
    CHECK(saw_request);
    CHECK(cp_auth_broker_busy(&broker));
    CHECK(broker.busy_retries > 0u);
    CHECK(!broker.pending);
    CHECK(cp_session_auth_state(&g_session) != CP_BOOT_AUTH_SUCCEEDED);
    {
        cp_oem_auth_async_completion completion;
        CHECK(cp_oem_auth_async_cancel(&broker.helper, 99u) ==
              CP_OEM_AUTH_ASYNC_OK);
        CHECK(cp_oem_auth_async_take_completion(&broker.helper, &completion));
        CHECK(completion.error == CP_OEM_AUTH_ASYNC_ERR_CANCELLED);
    }
    for (unsigned i = 0; i < 1000 && !broker.pending; i++) {
        pump_step(&g_session, &g_phone, 64u, true);
        CHECK(cp_auth_broker_service(&broker, 0, g_now) == CP_SESSION_OK);
    }
    CHECK(broker.pending);
    CHECK(!cp_auth_broker_busy(&broker));
    CHECK(cp_session_auth_state(&g_session) != CP_BOOT_AUTH_SUCCEEDED);
    cp_auth_broker_destroy(&broker, g_now);
    close(listener);
    CHECK(unlink(path) == 0 && rmdir(directory) == 0);
    puts("PASS: async helper/session integration preserves link service and cancels stale work");
    return 0;
}
