/*
    state.h - Umbriel snapshot parsing for wl_shimeji's Umbriel support plugin

    Copyright (C) 2026  luis

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
*/

#ifndef UMBRIEL_SUPPORT_STATE_H
#define UMBRIEL_SUPPORT_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jansson.h>

#define UMB_ID_LEN       72
#define UMB_MAX_WINDOWS  256
#define UMB_MAX_OUTPUTS  16

/* Fields we need from an Umbriel `windows` snapshot entry. */
typedef struct {
    char id[UMB_ID_LEN];
    char output[UMB_ID_LEN];
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    bool active;      /* seat-global keyboard focus */
    bool focused;     /* workspace-local remembered focus */
    bool visible;     /* on the active workspace, not minimized or tab-hidden */
    bool fullscreen;
    bool minimized;
    bool floating;
} umb_window_t;

/* Fields we need from an Umbriel `outputs` snapshot entry.
 * x/y/w/h describe the output's usable area in layout coordinates. */
typedef struct {
    char name[UMB_ID_LEN];
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
    bool enabled;
    bool powered;
} umb_output_t;

typedef struct {
    umb_window_t windows[UMB_MAX_WINDOWS];
    size_t window_count;
    umb_output_t outputs[UMB_MAX_OUTPUTS];
    size_t output_count;
    bool windows_ready;
    bool outputs_ready;
} umb_state_t;

/* Which window is published to wl_shimeji as the interactive element (IE)
 * the mascots can stand, sit and walk on. */
typedef enum {
    UMB_IE_MODE_ACTIVE,  /* seat-global focus, then workspace focus (default) */
    UMB_IE_MODE_FOCUSED, /* workspace focus first, then seat-global focus */
    UMB_IE_MODE_LARGEST, /* biggest eligible window on any workspace view */
    UMB_IE_MODE_NONE     /* never publish an IE */
} umb_ie_mode_t;

/* Forget every cached snapshot. */
void umb_state_reset(umb_state_t *state);

/* Apply one raw Umbriel event. `data` is the value of the event's "data" key.
 * Returns true when a windows or outputs snapshot was accepted. */
bool umb_state_apply(umb_state_t *state, const char *event, json_t *data);

/* Pick the window wl_shimeji should treat as the active interactive element:
 * the activated visible window, then the workspace-focused one, then the only
 * visible window. Fullscreen, minimized and hidden windows are ignored. */
bool umb_state_select_active(const umb_state_t *state, umb_window_t *out);

/* Same as umb_state_select_active(), but honours the configured IE mode. */
bool umb_state_select_active_mode(const umb_state_t *state, umb_ie_mode_t mode,
                                  umb_window_t *out);

/* Parse UMBRIEL_IE_MODE. Returns false for unknown values. */
bool umb_ie_mode_from_string(const char *text, umb_ie_mode_t *out);
const char *umb_ie_mode_name(umb_ie_mode_t mode);

/* Output lookup by connector name, or by a point in layout coordinates.
 * Both return -1 when there is no match. */
int umb_state_output_index(const umb_state_t *state, const char *name);
int umb_state_output_for_point(const umb_state_t *state, int32_t x, int32_t y);

/* True when the window's rectangle does not intersect any usable area. */
bool umb_state_window_offscreen(const umb_state_t *state, const umb_window_t *window);

#endif /* UMBRIEL_SUPPORT_STATE_H */
