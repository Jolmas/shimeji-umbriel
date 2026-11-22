/*
    state.c - Umbriel snapshot parsing for wl_shimeji's Umbriel support plugin

    Copyright (C) 2026  luis

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#include "state.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static void copy_string(char *dst, size_t size, const char *src)
{
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    snprintf(dst, size, "%s", src);
}

static const char *get_string(const json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key);
    return json_is_string(value) ? json_string_value(value) : NULL;
}

static bool get_bool(const json_t *object, const char *key, bool fallback)
{
    json_t *value = json_object_get(object, key);
    if (json_is_boolean(value)) {
        return json_is_true(value);
    }
    return fallback;
}

static int32_t get_int(const json_t *object, const char *key, int32_t fallback)
{
    json_t *value = json_object_get(object, key);
    int64_t number;

    if (json_is_integer(value)) {
        number = json_integer_value(value);
    } else if (json_is_real(value)) {
        number = (int64_t)json_real_value(value);
    } else {
        return fallback;
    }

    if (number > INT32_MAX) {
        return INT32_MAX;
    }
    if (number < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)number;
}

static void parse_window(umb_window_t *window, const json_t *entry)
{
    memset(window, 0, sizeof(*window));
    copy_string(window->id, sizeof(window->id), get_string(entry, "id"));
    copy_string(window->output, sizeof(window->output), get_string(entry, "output"));

    window->x = get_int(entry, "x", 0);
    window->y = get_int(entry, "y", 0);
    window->w = get_int(entry, "w", 0);
    window->h = get_int(entry, "h", 0);

    window->active = get_bool(entry, "active", false);
    window->focused = get_bool(entry, "focused", false);
    window->visible = get_bool(entry, "visible", true);
    window->fullscreen = get_bool(entry, "fullscreen", false);
    window->minimized = get_bool(entry, "minimized", false);
    window->floating = get_bool(entry, "floating", false);
}

static void parse_output(umb_output_t *output, const json_t *entry)
{
    const json_t *usable;
    const json_t *position;
    const json_t *logical;

    memset(output, 0, sizeof(*output));
    copy_string(output->name, sizeof(output->name), get_string(entry, "name"));
    output->enabled = get_bool(entry, "enabled", true);
    output->powered = get_bool(entry, "powered", true);

    usable = json_object_get(entry, "usable_area");
    if (json_is_object(usable)) {
        output->x = get_int(usable, "x", 0);
        output->y = get_int(usable, "y", 0);
        output->w = get_int(usable, "width", 0);
        output->h = get_int(usable, "height", 0);
        return;
    }

    /* Older/partial payloads: fall back to position + logical_size. */
    position = json_object_get(entry, "position");
    logical = json_object_get(entry, "logical_size");
    if (json_is_object(position)) {
        output->x = get_int(position, "x", 0);
        output->y = get_int(position, "y", 0);
    }
    if (json_is_object(logical)) {
        output->w = get_int(logical, "width", 0);
        output->h = get_int(logical, "height", 0);
    }
}

void umb_state_reset(umb_state_t *state)
{
    memset(state, 0, sizeof(*state));
}

bool umb_state_apply(umb_state_t *state, const char *event, json_t *data)
{
    size_t count;
    size_t i;

    if (state == NULL || event == NULL) {
        return false;
    }

    if (strcmp(event, "windows") == 0) {
        if (!json_is_array(data)) {
            return false;
        }
        count = json_array_size(data);
        if (count > UMB_MAX_WINDOWS) {
            count = UMB_MAX_WINDOWS;
        }
        state->window_count = count;
        for (i = 0; i < count; i++) {
            parse_window(&state->windows[i], json_array_get(data, i));
        }
        state->windows_ready = true;
        return true;
    }

    if (strcmp(event, "outputs") == 0) {
        if (!json_is_array(data)) {
            return false;
        }
        count = json_array_size(data);
        if (count > UMB_MAX_OUTPUTS) {
            count = UMB_MAX_OUTPUTS;
        }
        state->output_count = count;
        for (i = 0; i < count; i++) {
            parse_output(&state->outputs[i], json_array_get(data, i));
        }
        state->outputs_ready = true;
        return true;
    }

    return false;
}

static bool eligible(const umb_window_t *window)
{
    return window->visible && !window->fullscreen && !window->minimized &&
           window->w > 0 && window->h > 0;
}

static bool select_flagged(const umb_state_t *state, bool seat_focus,
                           umb_window_t *out)
{
    size_t i;

    for (i = 0; i < state->window_count; i++) {
        const umb_window_t *window = &state->windows[i];
        bool flagged = seat_focus ? window->active : window->focused;
        if (flagged && eligible(window)) {
            if (out != NULL) {
                *out = *window;
            }
            return true;
        }
    }
    return false;
}

static bool select_only_candidate(const umb_state_t *state, umb_window_t *out)
{
    size_t i;
    size_t candidates = 0;
    umb_window_t result;

    for (i = 0; i < state->window_count; i++) {
        if (eligible(&state->windows[i])) {
            result = state->windows[i];
            candidates++;
        }
    }
    if (candidates == 1 && out != NULL) {
        *out = result;
    }
    return candidates == 1;
}

static bool select_largest(const umb_state_t *state, umb_window_t *out)
{
    const umb_window_t *best = NULL;
    int64_t best_area = -1;
    int best_priority = -1;
    size_t i;

    for (i = 0; i < state->window_count; i++) {
        const umb_window_t *window = &state->windows[i];
        int64_t area;
        int priority;

        if (!eligible(window)) {
            continue;
        }
        area = (int64_t)window->w * window->h;
        /* Focus breaks ties so the choice stays stable between snapshots. */
        priority = (window->active ? 2 : 0) + (window->focused ? 1 : 0);
        if (area > best_area || (area == best_area && priority > best_priority)) {
            best = window;
            best_area = area;
            best_priority = priority;
        }
    }

    if (best == NULL) {
        return false;
    }
    if (out != NULL) {
        *out = *best;
    }
    return true;
}

bool umb_state_select_active_mode(const umb_state_t *state, umb_ie_mode_t mode,
                                  umb_window_t *out)
{
    if (state == NULL || mode == UMB_IE_MODE_NONE) {
        return false;
    }

    switch (mode) {
    case UMB_IE_MODE_FOCUSED:
        /* A panel or a layer may hold the keyboard focus; prefer the window
         * remembered by the workspace in that case. */
        if (select_flagged(state, false, out)) {
            return true;
        }
        return select_flagged(state, true, out) ||
               select_only_candidate(state, out);
    case UMB_IE_MODE_LARGEST:
        return select_largest(state, out);
    case UMB_IE_MODE_ACTIVE:
    default:
        /* Seat-global activation is the strongest signal. */
        if (select_flagged(state, true, out)) {
            return true;
        }
        return select_flagged(state, false, out) ||
               select_only_candidate(state, out);
    }
}

bool umb_state_select_active(const umb_state_t *state, umb_window_t *out)
{
    return umb_state_select_active_mode(state, UMB_IE_MODE_ACTIVE, out);
}

bool umb_ie_mode_from_string(const char *text, umb_ie_mode_t *out)
{
    if (text == NULL || out == NULL) {
        return false;
    }
    if (strcasecmp(text, "active") == 0 || strcasecmp(text, "seat") == 0) {
        *out = UMB_IE_MODE_ACTIVE;
    } else if (strcasecmp(text, "focused") == 0 ||
               strcasecmp(text, "workspace") == 0) {
        *out = UMB_IE_MODE_FOCUSED;
    } else if (strcasecmp(text, "largest") == 0 ||
               strcasecmp(text, "biggest") == 0 || strcasecmp(text, "max") == 0) {
        *out = UMB_IE_MODE_LARGEST;
    } else if (strcasecmp(text, "none") == 0 || strcasecmp(text, "off") == 0 ||
               strcasecmp(text, "disabled") == 0) {
        *out = UMB_IE_MODE_NONE;
    } else {
        return false;
    }
    return true;
}

const char *umb_ie_mode_name(umb_ie_mode_t mode)
{
    switch (mode) {
    case UMB_IE_MODE_FOCUSED:
        return "focused";
    case UMB_IE_MODE_LARGEST:
        return "largest";
    case UMB_IE_MODE_NONE:
        return "none";
    case UMB_IE_MODE_ACTIVE:
    default:
        return "active";
    }
}

int umb_state_output_index(const umb_state_t *state, const char *name)
{
    size_t i;

    if (state == NULL || name == NULL || name[0] == '\0') {
        return -1;
    }
    for (i = 0; i < state->output_count; i++) {
        if (strcmp(state->outputs[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int umb_state_output_for_point(const umb_state_t *state, int32_t x, int32_t y)
{
    size_t i;

    if (state == NULL) {
        return -1;
    }
    for (i = 0; i < state->output_count; i++) {
        const umb_output_t *output = &state->outputs[i];
        if (!output->enabled || !output->powered || output->w <= 0 || output->h <= 0) {
            continue;
        }
        if (x >= output->x && x < (int64_t)output->x + output->w &&
            y >= output->y && y < (int64_t)output->y + output->h) {
            return (int)i;
        }
    }
    return -1;
}

bool umb_state_window_offscreen(const umb_state_t *state, const umb_window_t *window)
{
    size_t i;

    if (state == NULL || window == NULL || window->w <= 0 || window->h <= 0) {
        return false;
    }
    for (i = 0; i < state->output_count; i++) {
        const umb_output_t *output = &state->outputs[i];
        if (!output->enabled || !output->powered || output->w <= 0 || output->h <= 0) {
            continue;
        }
        if ((int64_t)window->x < (int64_t)output->x + output->w &&
            (int64_t)window->x + window->w > output->x &&
            (int64_t)window->y < (int64_t)output->y + output->h &&
            (int64_t)window->y + window->h > output->y) {
            return false;
        }
    }
    return state->output_count > 0;
}
