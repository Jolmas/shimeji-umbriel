/*
    ipc.h - non-blocking client for Umbriel's UNIX socket

    Copyright (C) 2026  luis

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#ifndef UMBRIEL_SUPPORT_IPC_H
#define UMBRIEL_SUPPORT_IPC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jansson.h>

#define UMB_IPC_LINE_MAX 65536

typedef void (*umb_ipc_message_fn)(void *userdata, const char *event, json_t *data);
typedef void (*umb_ipc_reset_fn)(void *userdata);

typedef struct {
    int fd;
    char *socket_path;
    char buffer[UMB_IPC_LINE_MAX];
    size_t used;
    uint64_t retry_at_ns;
    unsigned failures;
    umb_ipc_message_fn on_message;
    umb_ipc_reset_fn on_reset;
    void *userdata;
} umb_ipc_t;

/* Build Umbriel's socket path: $UMBRIEL_SOCKET, or
 * $XDG_RUNTIME_DIR/umbriel-$WAYLAND_DISPLAY.sock. Caller frees the result. */
char *umb_ipc_default_socket_path(void);

bool umb_ipc_init(umb_ipc_t *ipc, const char *socket_path,
                  umb_ipc_message_fn on_message, umb_ipc_reset_fn on_reset,
                  void *userdata);
void umb_ipc_destroy(umb_ipc_t *ipc);

/* Connect (with backoff) and drain pending messages. Never blocks. */
void umb_ipc_tick(umb_ipc_t *ipc);
bool umb_ipc_connected(const umb_ipc_t *ipc);

/* Send one typed action request. `request` is not consumed or modified.
 * Noctalia/Umbriel's "cmd = action" wrapper is added here. */
bool umb_ipc_send_action(umb_ipc_t *ipc, const json_t *request);

#endif /* UMBRIEL_SUPPORT_IPC_H */
