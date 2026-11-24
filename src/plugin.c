/*
    plugin.c - wl_shimeji plugin entry points

    Talks to Umbriel over its UNIX socket and feeds wl_shimeji the geometry of
    the active window, plus optional move/restore/activate actions.

    Copyright (C) 2026  luis

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#define _GNU_SOURCE

#include <wl_shimeji/plugins.h>

#include <ctype.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

#include "ipc.h"
#include "state.h"

#define PLUGIN_NAME    "UmbrielSupport"
#define PLUGIN_VERSION "0.1.0"
#define PLUGIN_AUTHOR  "luis"

/* Optional settings file, shared with wl-shimeji-gui. Environment wins. */
#define IE_CONFIG_RELPATH ".config/wl_shimeji/umbriel-support.conf"

/* Umbriel reports the window frame top edge, while the visible surface starts
 * a few pixels lower. Move the interaction surface down so mascots stand
 * closer to the window instead of floating above its shadow. */
#define IE_TOP_INSET_DEFAULT 5

static plugin_t *g_plugin = NULL;
static set_active_ie_func g_set_active_ie = NULL;
static window_moved_hint_func g_window_moved_hint = NULL;
static umb_state_t g_state;
static umb_ipc_t g_ipc;
static umb_ie_mode_t g_ie_mode = UMB_IE_MODE_ACTIVE;
static int32_t g_ie_top_inset = IE_TOP_INSET_DEFAULT;

/* Last geometry published to wl_shimeji, used to detect moves. */
static bool g_last_valid = false;
static char g_last_id[UMB_ID_LEN];
static int32_t g_last_x;
static int32_t g_last_y;
static int32_t g_last_w;
static int32_t g_last_h;

static bool select_window(umb_window_t *out)
{
    return umb_state_select_active_mode(&g_state, g_ie_mode, out);
}

static void clear_active(void)
{
    if (g_last_valid && g_window_moved_hint != NULL) {
        g_window_moved_hint();
    }
    g_last_valid = false;
    g_last_id[0] = '\0';
    if (g_set_active_ie != NULL) {
        g_set_active_ie(false, 0, 0, 0, 0);
    }
}

static void publish_snapshot(void)
{
    umb_window_t active;
    bool have = select_window(&active);

    if (!have) {
        clear_active();
        return;
    }

    if (g_last_valid) {
        if (strcmp(g_last_id, active.id) != 0 || g_last_x != active.x ||
            g_last_y != active.y || g_last_w != active.w || g_last_h != active.h) {
            if (g_window_moved_hint != NULL) {
                g_window_moved_hint();
            }
        }
    }

    g_last_valid = true;
    snprintf(g_last_id, sizeof(g_last_id), "%s", active.id);
    g_last_x = active.x;
    g_last_y = active.y;
    g_last_w = active.w;
    g_last_h = active.h;

    if (g_set_active_ie != NULL) {
        int32_t inset = g_ie_top_inset;
        int32_t y = active.y;
        int32_t h = active.h;

        /* Un inset negativo sube la IE por encima del marco que informa
         * Umbriel; útil para que los pies coincidan con el borde visible. */
        if (inset != 0 && h > 1 && (inset > 0 ? inset < h : y > INT32_MIN - inset)) {
            y += inset;
            h -= inset;
        }

        g_set_active_ie(true, active.x, y, active.w, h);
    }
}

static void on_message(void *userdata, const char *event, json_t *data)
{
    (void)userdata;
    if (umb_state_apply(&g_state, event, data)) {
        publish_snapshot();
    }
}

static void on_reset(void *userdata)
{
    (void)userdata;
    umb_state_reset(&g_state);
    clear_active();
}

static void trim_spaces(char *text)
{
    char *start = text;
    char *end;

    while (isspace((unsigned char)*start)) {
        start++;
    }
    end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    if (start != text) {
        memmove(text, start, (size_t)(end - start) + 1);
    }
}

static char *ie_config_path(void)
{
    const char *base = getenv("XDG_CONFIG_HOME");
    char *path;
    size_t size;
    int written;

    if (base == NULL || base[0] == '\0') {
        base = getenv("HOME");
    }
    if (base == NULL || base[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        base = pw != NULL ? pw->pw_dir : NULL;
    }
    if (base == NULL || base[0] == '\0') {
        return NULL;
    }

    size = strlen(base) + 1 + strlen(IE_CONFIG_RELPATH) + 1;
    path = (char *)malloc(size);
    if (path == NULL) {
        return NULL;
    }
    written = snprintf(path, size, "%s/%s", base, IE_CONFIG_RELPATH);
    if (written < 0 || (size_t)written >= size) {
        free(path);
        return NULL;
    }
    return path;
}

static bool read_config_ie_mode(umb_ie_mode_t *out)
{
    char *path = ie_config_path();
    char line[512];
    FILE *file;
    bool found = false;

    if (path == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        free(path);
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment;
        char *equals;
        char *value;

        line[strcspn(line, "\r\n")] = '\0';
        comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }
        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        value = equals + 1;
        trim_spaces(line);
        trim_spaces(value);
        if (strcasecmp(line, "ie_mode") != 0 &&
            strcasecmp(line, "UMBRIEL_IE_MODE") != 0) {
            continue;
        }
        if (umb_ie_mode_from_string(value, out)) {
            found = true;
        } else {
            WARN("[UmbrielSupport] Invalid ie_mode '%s' in %s "
                 "(use active, focused, largest or none); using active",
                 value, path);
            *out = UMB_IE_MODE_ACTIVE;
        }
        break;
    }

    fclose(file);
    free(path);
    return found;
}

static void load_ie_mode(void)
{
    const char *value = getenv("UMBRIEL_IE_MODE");

    g_ie_mode = UMB_IE_MODE_ACTIVE;
    if (value != NULL && value[0] != '\0') {
        if (!umb_ie_mode_from_string(value, &g_ie_mode)) {
            WARN("[UmbrielSupport] Invalid UMBRIEL_IE_MODE '%s' "
                 "(use active, focused, largest or none); using active", value);
            g_ie_mode = UMB_IE_MODE_ACTIVE;
        }
    } else if (read_config_ie_mode(&g_ie_mode)) {
        INFO("[UmbrielSupport] IE mode: %s (umbriel-support.conf)",
             umb_ie_mode_name(g_ie_mode));
        return;
    }
    INFO("[UmbrielSupport] IE mode: %s", umb_ie_mode_name(g_ie_mode));
}

static bool read_config_top_inset(int32_t *out)
{
    char *path = ie_config_path();
    char line[512];
    FILE *file;
    bool found = false;

    if (path == NULL) {
        return false;
    }
    file = fopen(path, "r");
    if (file == NULL) {
        free(path);
        return false;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        char *equals;
        char *value;
        char *end;
        long number;

        if (comment != NULL) {
            *comment = '\0';
        }
        equals = strchr(line, '=');
        if (equals == NULL) {
            continue;
        }
        *equals = '\0';
        value = equals + 1;
        trim_spaces(line);
        trim_spaces(value);
        if (strcasecmp(line, "ie_top_inset") != 0 &&
            strcasecmp(line, "UMBRIEL_IE_TOP_INSET") != 0) {
            continue;
        }
        number = strtol(value, &end, 10);
        if (end != value && *end == '\0' && number >= -64 && number <= 64) {
            *out = (int32_t)number;
            found = true;
        } else {
            WARN("[UmbrielSupport] Invalid ie_top_inset '%s' in %s "
                 "(use an integer from -64 to 64); using %d",
                 value, path, IE_TOP_INSET_DEFAULT);
            *out = IE_TOP_INSET_DEFAULT;
        }
        break;
    }

    fclose(file);
    free(path);
    return found;
}

static void load_ie_top_inset(void)
{
    const char *value = getenv("UMBRIEL_IE_TOP_INSET");
    char *end;
    long number;

    g_ie_top_inset = IE_TOP_INSET_DEFAULT;
    if (value != NULL && value[0] != '\0') {
        number = strtol(value, &end, 10);
        if (end != value && *end == '\0' && number >= -64 && number <= 64) {
            g_ie_top_inset = (int32_t)number;
        } else {
            WARN("[UmbrielSupport] Invalid UMBRIEL_IE_TOP_INSET '%s' "
                 "(use an integer from -64 to 64); using %d",
                 value, IE_TOP_INSET_DEFAULT);
        }
    } else if (read_config_top_inset(&g_ie_top_inset)) {
        INFO("[UmbrielSupport] IE top inset: %d px (umbriel-support.conf)",
             g_ie_top_inset);
        return;
    }
    INFO("[UmbrielSupport] IE top inset: %d px", g_ie_top_inset);
}

static char *fallback_socket_path(void)
{
    const char *display = getenv("WAYLAND_DISPLAY");
    char *path;
    int written;
    size_t size;

    if (display == NULL || display[0] == '\0') {
        display = "wayland-0";
    }
    if (display[0] == '/') {
        const char *slash = strrchr(display, '/');
        display = slash != NULL ? slash + 1 : display;
    }

    size = strlen("/run/user/") + 32 + strlen("/umbriel-.sock") + strlen(display) + 1;
    path = (char *)malloc(size);
    if (path == NULL) {
        return NULL;
    }
    written = snprintf(path, size, "/run/user/%u/umbriel-%s.sock",
                       (unsigned)getuid(), display);
    if (written < 0 || (size_t)written >= size) {
        free(path);
        return NULL;
    }
    return path;
}

int wl_shimeji_plugin_init(plugin_t *self, set_cursor_pos_func set_cursor,
                           set_active_ie_func set_active_ie,
                           window_moved_hint_func window_moved_hint)
{
    char *socket_path;

    UNUSED(set_cursor); /* Umbriel's IPC has no global pointer query. */

    g_plugin = self;
    g_set_active_ie = set_active_ie;
    g_window_moved_hint = window_moved_hint;
    load_ie_mode();
    load_ie_top_inset();
    umb_state_reset(&g_state);
    g_last_valid = false;

    socket_path = umb_ipc_default_socket_path();
    if (socket_path == NULL) {
        socket_path = fallback_socket_path();
    }
    if (socket_path == NULL) {
        ERROR("[UmbrielSupport] Cannot determine Umbriel socket path");
        return -1;
    }

    if (!umb_ipc_init(&g_ipc, socket_path, on_message, on_reset, NULL)) {
        free(socket_path);
        return -1;
    }
    free(socket_path);

    return plugin_init(self, PLUGIN_NAME, PLUGIN_VERSION, PLUGIN_AUTHOR,
                       "Window interaction support for the Umbriel compositor",
                       version_to_i64(WL_SHIMEJI_PLUGIN_TARGET_VERSION));
}

int wl_shimeji_plugin_tick(plugin_t *self)
{
    UNUSED(self);
    umb_ipc_tick(&g_ipc);
    return 0;
}

int wl_shimeji_plugin_deinit(plugin_t *self)
{
    umb_ipc_destroy(&g_ipc);
    clear_active();
    return plugin_deinit(self);
}

static int send_window_action(const char *action, const char *anchor,
                              int32_t x, int32_t y, const char *window_id)
{
    json_t *request;
    bool sent;

    if (!umb_ipc_connected(&g_ipc) || window_id == NULL || window_id[0] == '\0') {
        return -1;
    }

    if (anchor != NULL) {
        request = json_pack("{s:s, s:s, s:i, s:i, s:s}",
                            "action", action,
                            "anchor", anchor,
                            "x", x,
                            "y", y,
                            "window", window_id);
    } else {
        request = json_pack("{s:s, s:s}",
                            "action", action,
                            "window", window_id);
    }
    if (request == NULL) {
        return -1;
    }

    sent = umb_ipc_send_action(&g_ipc, request);
    json_decref(request);
    return sent ? 0 : -1;
}

static int32_t clamp_i32(int64_t value)
{
    if (value > INT32_MAX) {
        return INT32_MAX;
    }
    if (value < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)value;
}

int wl_shimeji_plugin_move(plugin_t *self, int x, int y)
{
    umb_window_t active;
    int64_t center_x;
    int64_t center_y;
    int output_index;
    int64_t local_x;
    int64_t local_y;
    int result;

    UNUSED(self);

    if (!select_window(&active)) {
        return -1;
    }

    center_x = (int64_t)active.x + active.w / 2;
    center_y = (int64_t)active.y + active.h / 2;
    output_index = umb_state_output_for_point(&g_state,
                                               clamp_i32(center_x),
                                               clamp_i32(center_y));
    if (output_index < 0) {
        output_index = umb_state_output_index(&g_state, active.output);
    }
    if (output_index < 0 && g_state.output_count > 0) {
        output_index = 0;
    }

    local_x = x;
    local_y = y;
    if (output_index >= 0) {
        local_x -= g_state.outputs[output_index].x;
        local_y -= g_state.outputs[output_index].y;
    }

    /* Umbriel's window-move-to uses an anchor inside the output's usable area.
     * top_left + offsets is the closest match to wl_shimeji's global target. */
    result = send_window_action("window-move-to", "top_left",
                                clamp_i32(local_x), clamp_i32(local_y), active.id);
    if (result == 0 && g_window_moved_hint != NULL) {
        g_window_moved_hint();
    }
    return result;
}

int wl_shimeji_plugin_restore(plugin_t *self)
{
    size_t i;
    int failures = 0;
    int sent = 0;

    UNUSED(self);

    if (!umb_ipc_connected(&g_ipc) || !g_state.outputs_ready) {
        return -1;
    }

    for (i = 0; i < g_state.window_count; i++) {
        const umb_window_t *window = &g_state.windows[i];

        if (!window->floating || !umb_state_window_offscreen(&g_state, window)) {
            continue;
        }
        /* Offsets are relative to the window's own output usable area. */
        if (send_window_action("window-move-to", "top_left", 100, 100,
                               window->id) == 0) {
            sent++;
        } else {
            failures++;
        }
    }

    if (sent > 0 && g_window_moved_hint != NULL) {
        g_window_moved_hint();
    }
    return failures > 0 ? -1 : 0;
}

int wl_shimeji_plugin_activate(plugin_t *self)
{
    umb_window_t active;

    UNUSED(self);

    if (!select_window(&active)) {
        return -1;
    }
    return send_window_action("window-focus", NULL, 0, 0, active.id);
}
