/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 * Offline-tested integration; not activated by the product daemon. */
#ifndef CP_AUTH_BROKER_H
#define CP_AUTH_BROKER_H
#include "cp_session.h"
#include "cp_oem_auth_async.h"

/* Caller owns the session and poll loop. service() advances link deadlines
 * before nonblocking helper I/O; helper completion never means phone auth or
 * projection succeeded. One operation, fixed storage, no credential logging. */
typedef struct {
    bool initialized;
    bool pending;
    /* Helper/provider reported busy (for example a semaphore timeout).  This
     * is never authentication success and never releases credentials. */
    bool busy;
    uint32_t busy_retries;
    cp_session_t *session;
    cp_oem_auth_async helper;
    cp_boot_auth_request request;
    uint32_t request_id;
} cp_auth_broker;

cp_session_error_t cp_auth_broker_init(cp_auth_broker *broker,
    cp_session_t *session, const cp_oem_auth_async_config *config);
cp_session_error_t cp_auth_broker_service(cp_auth_broker *broker,
    short helper_revents, uint64_t now_ms);
int cp_auth_broker_pollfd(const cp_auth_broker *broker, struct pollfd *pfd);
uint64_t cp_auth_broker_next_deadline(const cp_auth_broker *broker);
/* True while the provider refused a request as busy.  Busy is pending work,
 * not an authentication result; the caller must not release credentials. */
bool cp_auth_broker_busy(const cp_auth_broker *broker);
/* Destroy before destroying the borrowed session; pending auth is failed,
 * never silently switched back to a synchronous provider. */
void cp_auth_broker_destroy(cp_auth_broker *broker, uint64_t now_ms);
#endif
