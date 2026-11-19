/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only */
#include "cp_auth_broker.h"
#include <string.h>

static bool same_request(const cp_boot_auth_request *a,
                         const cp_boot_auth_request *b)
{
    return a->op == b->op && a->generation == b->generation &&
           a->token == b->token;
}

static void retire(cp_auth_broker *broker)
{
    cp_oem_auth_async_completion completion;
    if (broker->pending)
        (void)cp_oem_auth_async_cancel(&broker->helper,
                                      broker->request.generation);
    (void)cp_oem_auth_async_take_completion(&broker->helper, &completion);
    memset(&completion, 0, sizeof(completion));
    memset(&broker->request, 0, sizeof(broker->request));
    broker->pending = false;
    broker->busy = false;
    broker->request_id = 0u;
}

cp_session_error_t cp_auth_broker_init(cp_auth_broker *broker,
    cp_session_t *session, const cp_oem_auth_async_config *config)
{
    cp_session_error_t error;
    if (broker == NULL || session == NULL || config == NULL)
        return CP_SESSION_ERR_ARGUMENT;
    memset(broker, 0, sizeof(*broker));
    if (cp_oem_auth_async_init(&broker->helper, config) != CP_OEM_AUTH_ASYNC_OK)
        return CP_SESSION_ERR_PROVIDER;
    error = cp_session_set_async_auth(session, true);
    if (error != CP_SESSION_OK) {
        cp_oem_auth_async_destroy(&broker->helper);
        return error;
    }
    broker->session = session;
    broker->initialized = true;
    return CP_SESSION_OK;
}

cp_session_error_t cp_auth_broker_service(cp_auth_broker *broker,
    short helper_revents, uint64_t now_ms)
{
    cp_boot_auth_request request;
    cp_oem_auth_async_completion completion;
    cp_session_error_t error;
    cp_oem_auth_async_error helper_error;
    if (broker == NULL || !broker->initialized)
        return CP_SESSION_ERR_ARGUMENT;
    error = cp_session_tick(broker->session, now_ms);
    if (error == CP_SESSION_ERR_CLOCK_ROLLBACK) return error;
    bool have_request = cp_session_auth_request(broker->session, &request);
    if (broker->pending && (!have_request ||
        !same_request(&request, &broker->request))) {
        retire(broker);
        helper_revents = 0;
    }
    if (error != CP_SESSION_OK || !have_request) return error;
    if (!broker->pending) {
        helper_error = request.op == CP_BOOT_AUTH_OP_CERTIFICATE ?
            cp_oem_auth_async_begin_certificate(&broker->helper,
                request.generation, now_ms, request.deadline_ms) :
            cp_oem_auth_async_begin_signature(&broker->helper,
                request.generation, request.challenge, request.challenge_len,
                now_ms, request.deadline_ms);
        if (helper_error != CP_OEM_AUTH_ASYNC_OK) {
            if (helper_error == CP_OEM_AUTH_ASYNC_ERR_BUSY) {
                /* Provider/semaphore busy is a pending condition, never a
                 * success and never an immediate credential failure.  Retry
                 * until the generation-bound request deadline expires. */
                broker->busy = true;
                broker->busy_retries++;
                return CP_SESSION_OK;
            }
            return cp_session_auth_complete(broker->session, &request, false,
                                             NULL, 0u, now_ms);
        }
        broker->pending = true;
        broker->busy = false;
        broker->request = request;
        broker->request_id = broker->helper.request_id;
        helper_revents = 0; /* poll events cannot belong to a newly opened fd */
    }
    (void)cp_oem_auth_async_service(&broker->helper, helper_revents, now_ms);
    if (!cp_oem_auth_async_take_completion(&broker->helper, &completion))
        return CP_SESSION_OK;
    bool valid = completion.error == CP_OEM_AUTH_ASYNC_OK &&
        completion.generation == request.generation &&
        completion.request_id == broker->request_id &&
        ((request.op == CP_BOOT_AUTH_OP_CERTIFICATE &&
          completion.op == CP_OEM_AUTH_ASYNC_OP_CERTIFICATE) ||
         (request.op == CP_BOOT_AUTH_OP_SIGNATURE &&
          completion.op == CP_OEM_AUTH_ASYNC_OP_SIGNATURE));
    error = cp_session_auth_complete(broker->session, &request, valid,
                                     completion.bytes, completion.length, now_ms);
    memset(&completion, 0, sizeof(completion));
    retire(broker);
    return error;
}

int cp_auth_broker_pollfd(const cp_auth_broker *broker, struct pollfd *pfd)
{
    if (broker == NULL || !broker->initialized || pfd == NULL) return -1;
    pfd->fd = cp_oem_auth_async_get_fd(&broker->helper);
    pfd->events = cp_oem_auth_async_get_poll_events(&broker->helper);
    pfd->revents = 0;
    return pfd->fd >= 0 ? 1 : 0;
}

uint64_t cp_auth_broker_next_deadline(const cp_auth_broker *broker)
{
    if (broker == NULL || !broker->initialized) return UINT64_MAX;
    uint64_t deadline = cp_session_next_deadline(broker->session);
    if (broker->pending && broker->request.deadline_ms < deadline)
        deadline = broker->request.deadline_ms;
    return deadline;
}

bool cp_auth_broker_busy(const cp_auth_broker *broker)
{
    return broker != NULL && broker->initialized && broker->busy;
}

void cp_auth_broker_destroy(cp_auth_broker *broker, uint64_t now_ms)
{
    cp_boot_auth_request request;
    if (broker == NULL || !broker->initialized) return;
    if (now_ms < broker->session->last_now_ms)
        now_ms = broker->session->last_now_ms;
    if (cp_session_auth_request(broker->session, &request))
        (void)cp_session_auth_complete(broker->session, &request,
                                       false, NULL, 0u, now_ms);
    retire(broker);
    cp_oem_auth_async_destroy(&broker->helper);
    memset(broker, 0, sizeof(*broker));
}
