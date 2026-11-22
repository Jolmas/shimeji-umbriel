/*
    ipc.c - non-blocking client for Umbriel's UNIX socket

    Copyright (C) 2026  luis

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#define _GNU_SOURCE

#include "ipc.h"

#include <wl_shimeji/plugins.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

static const char SUBSCRIBE_REQUEST[] =
    "{\"cmd\":\"subscribe\",\"events\":[\"windows\",\"outputs\"]}\n";

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static uint64_t backoff_ns(unsigned failures)
{
    uint64_t seconds = 1;
    unsigned shift = failures;
    if (shift > 5) {
        shift = 5;
    }
    while (shift-- > 0) {
        seconds *= 2;
    }
    if (seconds > 30) {
        seconds = 30;
    }
    return seconds * 1000000000ULL;
}

char *umb_ipc_default_socket_path(void)
{
    const char *override = getenv("UMBRIEL_SOCKET");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *display = getenv("WAYLAND_DISPLAY");
    char *path;
    size_t size;
    int written;

    if (override != NULL && override[0] != '\0') {
        return strdup(override);
    }
    if (runtime == NULL || runtime[0] == '\0' || display == NULL || display[0] == '\0') {
        return NULL;
    }

    /* WAYLAND_DISPLAY may be an absolute path; Umbriel's socket name uses
     * only its file name in that case. */
    if (display[0] == '/') {
        const char *slash = strrchr(display, '/');
        display = slash != NULL ? slash + 1 : display;
    }

    size = strlen(runtime) + strlen(display) + strlen("/umbriel-.sock") + 1;
    path = (char *)malloc(size);
    if (path == NULL) {
        return NULL;
    }
    written = snprintf(path, size, "%s/umbriel-%s.sock", runtime, display);
    if (written < 0 || (size_t)written >= size) {
        free(path);
        return NULL;
    }
    return path;
}

static bool send_all(umb_ipc_t *ipc, const char *data, size_t length)
{
    size_t offset = 0;

    while (offset < length) {
        struct pollfd pfd;
        ssize_t sent;
        int ready;

        pfd.fd = ipc->fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        ready = poll(&pfd, 1, 200);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            return false;
        }
        sent = send(ipc->fd, data + offset, length - offset, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            return false;
        }
        offset += (size_t)sent;
    }
    return true;
}

static void mark_disconnected(umb_ipc_t *ipc, const char *reason)
{
    if (ipc->fd >= 0) {
        close(ipc->fd);
        ipc->fd = -1;
    }
    ipc->used = 0;
    ipc->retry_at_ns = now_ns() + backoff_ns(ipc->failures++);
    WARN("[UmbrielSupport] IPC connection lost (%s); retrying later", reason);
    if (ipc->on_reset != NULL) {
        ipc->on_reset(ipc->userdata);
    }
}

static bool connect_now(umb_ipc_t *ipc)
{
    struct sockaddr_un address;
    struct pollfd pfd;
    socklen_t length;
    int error = 0;
    int fd;

    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", ipc->socket_path);
    length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + strlen(address.sun_path) + 1);

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        WARN("[UmbrielSupport] Cannot create UNIX socket: %s", strerror(errno));
        ipc->retry_at_ns = now_ns() + backoff_ns(ipc->failures++);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&address, length) < 0) {
        if (errno == EINPROGRESS) {
            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            if (poll(&pfd, 1, 200) <= 0) {
                close(fd);
                ipc->retry_at_ns = now_ns() + backoff_ns(ipc->failures++);
                return false;
            }
            error = 0;
            length = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
        } else {
            error = errno;
        }
        if (error != 0) {
            close(fd);
            ipc->retry_at_ns = now_ns() + backoff_ns(ipc->failures++);
            return false;
        }
    }

    ipc->fd = fd;
    ipc->used = 0;
    if (!send_all(ipc, SUBSCRIBE_REQUEST, sizeof(SUBSCRIBE_REQUEST) - 1)) {
        mark_disconnected(ipc, "subscribe failed");
        return false;
    }

    ipc->failures = 0;
    INFO("[UmbrielSupport] Connected to %s", ipc->socket_path);
    if (ipc->on_reset != NULL) {
        ipc->on_reset(ipc->userdata);
    }
    return true;
}

static void process_line(umb_ipc_t *ipc, const char *line, size_t length)
{
    json_error_t error;
    json_t *message;
    json_t *event;
    json_t *err;

    message = json_loadb(line, length, 0, &error);
    if (message == NULL) {
        WARN("[UmbrielSupport] Invalid JSON from Umbriel: %s", error.text);
        return;
    }
    if (!json_is_object(message)) {
        json_decref(message);
        return;
    }

    event = json_object_get(message, "event");
    if (json_is_string(event) && ipc->on_message != NULL) {
        ipc->on_message(ipc->userdata, json_string_value(event),
                        json_object_get(message, "data"));
    } else {
        err = json_object_get(message, "err");
        if (json_is_string(err)) {
            WARN("[UmbrielSupport] Umbriel replied: %s", json_string_value(err));
        }
    }
    json_decref(message);
}

static void drain_socket(umb_ipc_t *ipc)
{
    char *newline;

    while ((newline = memchr(ipc->buffer, '\n', ipc->used)) != NULL) {
        size_t length = (size_t)(newline - ipc->buffer);
        process_line(ipc, ipc->buffer, length);
        ipc->used -= length + 1;
        memmove(ipc->buffer, newline + 1, ipc->used);
    }
}

bool umb_ipc_init(umb_ipc_t *ipc, const char *socket_path,
                  umb_ipc_message_fn on_message, umb_ipc_reset_fn on_reset,
                  void *userdata)
{
    if (ipc == NULL || socket_path == NULL) {
        return false;
    }
    memset(ipc, 0, sizeof(*ipc));
    ipc->fd = -1;
    ipc->socket_path = strdup(socket_path);
    if (ipc->socket_path == NULL) {
        return false;
    }
    ipc->on_message = on_message;
    ipc->on_reset = on_reset;
    ipc->userdata = userdata;
    return true;
}

void umb_ipc_destroy(umb_ipc_t *ipc)
{
    if (ipc == NULL) {
        return;
    }
    if (ipc->fd >= 0) {
        close(ipc->fd);
        ipc->fd = -1;
    }
    free(ipc->socket_path);
    ipc->socket_path = NULL;
    ipc->used = 0;
}

bool umb_ipc_connected(const umb_ipc_t *ipc)
{
    return ipc != NULL && ipc->fd >= 0;
}

void umb_ipc_tick(umb_ipc_t *ipc)
{
    struct pollfd pfd;
    ssize_t received;
    int ready;

    if (ipc == NULL) {
        return;
    }
    if (ipc->fd < 0) {
        if (now_ns() >= ipc->retry_at_ns) {
            connect_now(ipc);
        }
        return;
    }

    pfd.fd = ipc->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    ready = poll(&pfd, 1, 0);
    if (ready < 0) {
        if (errno == EINTR) {
            return;
        }
        mark_disconnected(ipc, "poll failed");
        return;
    }
    if (ready == 0) {
        return;
    }

    if (pfd.revents & POLLIN) {
        received = recv(ipc->fd, ipc->buffer + ipc->used,
                        UMB_IPC_LINE_MAX - ipc->used, 0);
        if (received == 0) {
            mark_disconnected(ipc, "socket closed");
            return;
        }
        if (received < 0) {
            if (errno != EAGAIN && errno != EINTR) {
                mark_disconnected(ipc, "read failed");
            }
            return;
        }
        ipc->used += (size_t)received;
        drain_socket(ipc);
        if (ipc->used == UMB_IPC_LINE_MAX) {
            WARN("[UmbrielSupport] IPC line too long, discarding");
            ipc->used = 0;
        }
    }
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        mark_disconnected(ipc, "socket error");
    }
}

bool umb_ipc_send_action(umb_ipc_t *ipc, const json_t *request)
{
    json_t *wrapped;
    char *text;
    bool ok;

    if (!umb_ipc_connected(ipc) || !json_is_object(request)) {
        return false;
    }

    wrapped = json_deep_copy(request);
    if (wrapped == NULL) {
        return false;
    }
    if (json_object_set_new(wrapped, "cmd", json_string("action")) != 0) {
        json_decref(wrapped);
        return false;
    }

    text = json_dumps(wrapped, JSON_COMPACT);
    json_decref(wrapped);
    if (text == NULL) {
        return false;
    }

    size_t length = strlen(text);
    char *line = (char *)malloc(length + 2);
    if (line == NULL) {
        free(text);
        return false;
    }
    memcpy(line, text, length);
    line[length] = '\n';
    line[length + 1] = '\0';
    free(text);

    ok = send_all(ipc, line, length + 1);
    free(line);
    return ok;
}
