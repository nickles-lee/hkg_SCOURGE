/* SPDX-FileCopyrightText: 2026 The SCOURGE Project contributors
 * SPDX-License-Identifier: AGPL-3.0-only
 */

/*
 * Minimal session-bus stand-in for com.lge.avn.applauncher.
 *
 * AppDMClientEngine launches the updater by calling
 *   com.lge.avn.applauncher.request.launch(int appid, string appname,
 *                                           string applocation)
 * at /Request. The real AppLauncher hands the request to ApplicationManager.
 * This stub just logs the request and execs applocation, which is enough to
 * observe /update/usb/bin/AppUpgrade in the research VM.
 */
#include <dbus/dbus.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVICE "com.lge.avn.applauncher"
#define PATH    "/Request"
#define IFACE   "com.lge.avn.applauncher.request"

static volatile sig_atomic_t keep_running = 1;
static pid_t child_pid;
static char **child_args;
static int child_argc;

static void stop_handler(int signo)
{
    (void)signo;
    keep_running = 0;
}

static int send_message(DBusConnection *connection, DBusMessage *reply)
{
    if (reply == NULL)
        return 0;
    if (!dbus_connection_send(connection, reply, NULL)) {
        dbus_message_unref(reply);
        return 0;
    }
    dbus_connection_flush(connection);
    dbus_message_unref(reply);
    return 1;
}

static void reap_child(void)
{
    int status;
    if (child_pid <= 0 || waitpid(child_pid, &status, WNOHANG) != child_pid)
        return;
    fprintf(stderr, "APPLAUNCHER_STUB_EXIT pid=%ld status=%d\n", (long)child_pid, status);
    fflush(stderr);
    child_pid = 0;
}

static int launch_child(const char *appname, const char *location)
{
    pid_t pid;
    int fd, errors[2], child_error = 0;
    ssize_t count;
    reap_child();
    if (location == NULL || strcmp(location, "/update/usb/bin/AppUpgrade") != 0 ||
        appname == NULL || strcmp(appname, "AppUpgrade") != 0)
        return 0; /* This is deliberately not a general command-execution API. */
    if (child_pid > 0) {
        fprintf(stderr, "APPLAUNCHER_STUB_ALREADY_RUNNING pid=%ld\n", (long)child_pid);
        fflush(stderr);
        return 1;
    }
    if (pipe(errors) != 0)
        return 0;
    fcntl(errors[1], F_SETFD, FD_CLOEXEC);
    pid = fork();
    if (pid < 0) {
        close(errors[0]); close(errors[1]);
        return 0;
    }
    if (pid > 0) {
        close(errors[1]);
        do { count = read(errors[0], &child_error, sizeof(child_error)); }
        while (count < 0 && errno == EINTR);
        close(errors[0]);
        if (count != 0) {
            fprintf(stderr, "APPLAUNCHER_STUB_EXEC_FAIL pid=%ld errno=%d\n", (long)pid, child_error);
            waitpid(pid, NULL, 0);
            fflush(stderr);
            return 0;
        }
        child_pid = pid;
        fprintf(stderr, "APPLAUNCHER_STUB_EXEC_OK pid=%ld app=%s path=%s\n",
                (long)pid, appname, location);
        /* Survival is a separate, deliberately weaker milestone than readiness. */
        for (int i = 0; i < 20 && child_pid > 0; ++i) {
            struct timespec delay = {0, 100000000};
            nanosleep(&delay, NULL);
            reap_child();
        }
        if (child_pid > 0)
            fprintf(stderr, "APPLAUNCHER_STUB_ALIVE pid=%ld after_ms=2000\n", (long)pid);
        fflush(stderr);
        return child_pid > 0;
    }
    close(errors[0]);
    setsid();
    if (chdir("/app/bin") != 0)
        goto failed;
    fd = open("/tmp/appupgrade.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
    }
    char **argv = calloc((size_t)child_argc + 2, sizeof(char *));
    if (argv == NULL)
        goto failed;
    argv[0] = (char *)location;
    for (int i = 0; i < child_argc; ++i)
        argv[i + 1] = child_args[i];
    fprintf(stderr, "APPLAUNCHER_STUB_CONTEXT cwd=/app/bin argc=%d\n", child_argc + 1);
    for (int i = 0; i < child_argc + 1; ++i)
        fprintf(stderr, "APPLAUNCHER_STUB_ARG index=%d value=%s\n", i, argv[i]);
    const char *keys[] = {"HOME", "LD_LIBRARY_PATH", "QT_QPA_PLATFORM", "QT_QUICK_BACKEND",
                          "XDG_RUNTIME_DIR", "WAYLAND_DISPLAY", "DBUS_SYSTEM_BUS_ADDRESS",
                          "DBUS_SESSION_BUS_ADDRESS", NULL};
    for (int i = 0; keys[i] != NULL; ++i) {
        const char *value = getenv(keys[i]);
        fprintf(stderr, "APPLAUNCHER_STUB_ENV %s=%s\n", keys[i], value ? value : "(unset)");
    }
    fflush(stderr);
    execv(location, argv);
failed:
    child_error = errno;
    (void)write(errors[1], &child_error, sizeof(child_error));
    _exit(127);
}

int main(int argc, char **argv)
{
    DBusConnection *connection;
    DBusError error;
    int request_result;
    child_argc = argc - 1;
    child_args = argv + 1;

    dbus_error_init(&error);
    connection = dbus_bus_get(DBUS_BUS_SESSION, &error);
    if (connection == NULL) {
        fprintf(stderr, "applauncher-stub: session bus: %s: %s\n",
                error.name ? error.name : "unknown",
                error.message ? error.message : "unknown");
        dbus_error_free(&error);
        return 2;
    }
    dbus_connection_set_exit_on_disconnect(connection, FALSE);
    request_result = dbus_bus_request_name(connection, SERVICE,
                                           DBUS_NAME_FLAG_DO_NOT_QUEUE, &error);
    if (dbus_error_is_set(&error)) {
        fprintf(stderr, "applauncher-stub: request_name: %s: %s\n",
                error.name, error.message);
        dbus_error_free(&error);
        dbus_connection_unref(connection);
        return 3;
    }
    if (request_result != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER &&
        request_result != DBUS_REQUEST_NAME_REPLY_ALREADY_OWNER) {
        fprintf(stderr, "applauncher-stub: name busy rc=%d\n", request_result);
        dbus_connection_unref(connection);
        return 4;
    }

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    fprintf(stderr, "APPLAUNCHER_STUB_READY service=%s\n", SERVICE);
    fflush(stderr);

    while (keep_running && dbus_connection_get_is_connected(connection)) {
        DBusMessage *request;
        const char *member;
        const char *iface;

        reap_child();
        dbus_connection_read_write(connection, 100);
        request = dbus_connection_pop_message(connection);
        if (request == NULL)
            continue;
        if (dbus_message_get_type(request) != DBUS_MESSAGE_TYPE_METHOD_CALL) {
            dbus_message_unref(request);
            continue;
        }
        member = dbus_message_get_member(request);
        iface = dbus_message_get_interface(request);
        fprintf(stderr,
                "APPLAUNCHER_STUB_CALL path=%s interface=%s member=%s signature=%s\n",
                dbus_message_get_path(request),
                iface ? iface : "(null)", member ? member : "(null)",
                dbus_message_get_signature(request));
        fflush(stderr);

        if (dbus_message_is_method_call(request, "org.freedesktop.DBus.Peer",
                                        "Ping")) {
            send_message(connection, dbus_message_new_method_return(request));
        } else if (dbus_message_is_method_call(
                       request, "org.freedesktop.DBus.Introspectable",
                       "Introspect")) {
            const char *xml =
                "<node><interface name=\"" IFACE "\">"
                "<method name=\"launch\">"
                "<arg direction=\"in\" type=\"i\" name=\"appid\"/>"
                "<arg direction=\"in\" type=\"s\" name=\"appname\"/>"
                "<arg direction=\"in\" type=\"s\" name=\"applocation\"/>"
                "</method></interface></node>";
            DBusMessage *reply = dbus_message_new_method_return(request);
            if (reply != NULL &&
                dbus_message_append_args(reply, DBUS_TYPE_STRING, &xml,
                                         DBUS_TYPE_INVALID))
                send_message(connection, reply);
            else if (reply != NULL)
                dbus_message_unref(reply);
        } else if (dbus_message_has_path(request, PATH) &&
                   dbus_message_is_method_call(request, IFACE, "launch") &&
                   strcmp(dbus_message_get_signature(request), "iss") == 0) {
            DBusMessageIter it;
            int32_t appid = -1;
            const char *appname = NULL;
            const char *location = NULL;
            if (dbus_message_iter_init(request, &it)) {
                dbus_message_iter_get_basic(&it, &appid);
                if (dbus_message_iter_next(&it)) {
                    dbus_message_iter_get_basic(&it, &appname);
                    if (dbus_message_iter_next(&it))
                        dbus_message_iter_get_basic(&it, &location);
                }
            }
            fprintf(stderr, "APPLAUNCHER_STUB_LAUNCH appid=%d app=%s path=%s\n",
                    appid, appname ? appname : "(null)",
                    location ? location : "(null)");
            fflush(stderr);
            if (appid == 300 && launch_child(appname, location))
                send_message(connection, dbus_message_new_method_return(request));
            else
                send_message(connection, dbus_message_new_error(request, DBUS_ERROR_FAILED,
                             "research updater launch failed or unsupported"));
        } else if (dbus_message_has_path(request, "/Inform") &&
                   dbus_message_is_method_call(request, "com.lge.avn.applauncher.inform", "launched") &&
                   strcmp(dbus_message_get_signature(request), "is") == 0) {
            send_message(connection, dbus_message_new_method_return(request));
        } else {
            send_message(connection, dbus_message_new_error(request, DBUS_ERROR_UNKNOWN_METHOD,
                         "method not implemented by research launcher"));
        }
        dbus_message_unref(request);
    }

    if (child_pid > 0) {
        kill(-child_pid, SIGKILL);
        waitpid(child_pid, NULL, 0);
    }
    dbus_bus_release_name(connection, SERVICE, NULL);
    dbus_connection_unref(connection);
    return 0;
}
