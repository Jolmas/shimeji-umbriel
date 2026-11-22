/*
    test_state.c - offline checks for Umbriel snapshot parsing

    Run with: make test
*/

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "state.h"

static json_t *parse(const char *text)
{
    json_error_t error;
    json_t *value = json_loads(text, 0, &error);
    if (value == NULL) {
        fprintf(stderr, "bad test JSON: %s\n", error.text);
        abort();
    }
    return value;
}

static void test_windows(void)
{
    umb_state_t state;
    umb_window_t active;
    json_t *data;

    data = parse("["
        "{\"id\":\"full\",\"x\":0,\"y\":0,\"w\":1920,\"h\":1080,"
        "\"active\":true,\"fullscreen\":true,\"visible\":true,\"output\":\"DP-1\"},"
        "{\"id\":\"term\",\"x\":100,\"y\":200,\"w\":800,\"h\":600,"
        "\"active\":false,\"focused\":true,\"visible\":true,\"output\":\"DP-1\"},"
        "{\"id\":\"hidden\",\"x\":0,\"y\":0,\"w\":400,\"h\":300,"
        "\"visible\":false,\"output\":\"DP-1\"}"
    "]");

    umb_state_reset(&state);
    assert(umb_state_apply(&state, "windows", data));
    assert(state.window_count == 3);
    assert(umb_state_select_active(&state, &active));
    assert(strcmp(active.id, "term") == 0);
    assert(active.x == 100 && active.y == 200);
    json_decref(data);

    data = parse("[{\"id\":\"fs\",\"x\":0,\"y\":0,\"w\":1920,\"h\":1080,"
                 "\"active\":true,\"fullscreen\":true,\"visible\":true}]");
    assert(umb_state_apply(&state, "windows", data));
    assert(!umb_state_select_active(&state, &active));
    json_decref(data);
}

static void test_ie_modes(void)
{
    umb_state_t state;
    umb_window_t window;
    umb_ie_mode_t mode;
    json_t *data;

    data = parse("["
        "{\"id\":\"small\",\"x\":10,\"y\":20,\"w\":100,\"h\":100,"
        "\"active\":true,\"visible\":true},"
        "{\"id\":\"medium\",\"x\":30,\"y\":40,\"w\":400,\"h\":300,"
        "\"focused\":true,\"visible\":true},"
        "{\"id\":\"large\",\"x\":50,\"y\":60,\"w\":1000,\"h\":800,"
        "\"visible\":true}"
    "]");
    umb_state_reset(&state);
    assert(umb_state_apply(&state, "windows", data));

    assert(umb_state_select_active_mode(&state, UMB_IE_MODE_ACTIVE, &window));
    assert(strcmp(window.id, "small") == 0);
    assert(umb_state_select_active_mode(&state, UMB_IE_MODE_FOCUSED, &window));
    assert(strcmp(window.id, "medium") == 0);
    assert(umb_state_select_active_mode(&state, UMB_IE_MODE_LARGEST, &window));
    assert(strcmp(window.id, "large") == 0);
    assert(!umb_state_select_active_mode(&state, UMB_IE_MODE_NONE, &window));
    json_decref(data);

    assert(umb_ie_mode_from_string("active", &mode) && mode == UMB_IE_MODE_ACTIVE);
    assert(umb_ie_mode_from_string("Seat", &mode) && mode == UMB_IE_MODE_ACTIVE);
    assert(umb_ie_mode_from_string("focused", &mode) && mode == UMB_IE_MODE_FOCUSED);
    assert(umb_ie_mode_from_string("workspace", &mode) && mode == UMB_IE_MODE_FOCUSED);
    assert(umb_ie_mode_from_string("largest", &mode) && mode == UMB_IE_MODE_LARGEST);
    assert(umb_ie_mode_from_string("max", &mode) && mode == UMB_IE_MODE_LARGEST);
    assert(umb_ie_mode_from_string("none", &mode) && mode == UMB_IE_MODE_NONE);
    assert(umb_ie_mode_from_string("off", &mode) && mode == UMB_IE_MODE_NONE);
    assert(!umb_ie_mode_from_string("bogus", &mode));
    assert(!umb_ie_mode_from_string(NULL, &mode));
    assert(strcmp(umb_ie_mode_name(UMB_IE_MODE_ACTIVE), "active") == 0);
    assert(strcmp(umb_ie_mode_name(UMB_IE_MODE_FOCUSED), "focused") == 0);
    assert(strcmp(umb_ie_mode_name(UMB_IE_MODE_LARGEST), "largest") == 0);
    assert(strcmp(umb_ie_mode_name(UMB_IE_MODE_NONE), "none") == 0);
}

static void test_outputs(void)
{
    umb_state_t state;
    json_t *data;
    int index;

    data = parse("["
        "{\"name\":\"DP-1\",\"enabled\":true,\"powered\":true,"
        "\"position\":{\"x\":0,\"y\":0},\"logical_size\":{\"width\":1920,\"height\":1080},"
        "\"usable_area\":{\"x\":0,\"y\":30,\"width\":1920,\"height\":1050}},"
        "{\"name\":\"HDMI-A-1\",\"enabled\":true,\"powered\":true,"
        "\"usable_area\":{\"x\":1920,\"y\":0,\"width\":1280,\"height\":1024}}"
    "]");

    umb_state_reset(&state);
    assert(umb_state_apply(&state, "outputs", data));
    assert(state.output_count == 2);
    assert(state.outputs[0].y == 30 && state.outputs[0].h == 1050);
    index = umb_state_output_index(&state, "HDMI-A-1");
    assert(index == 1);
    assert(umb_state_output_index(&state, "missing") == -1);
    assert(umb_state_output_for_point(&state, 2000, 100) == 1);
    assert(umb_state_output_for_point(&state, 100, 500) == 0);
    assert(umb_state_output_for_point(&state, -10, 500) == -1);
    json_decref(data);
}

static void test_offscreen(void)
{
    umb_state_t state;
    umb_window_t window;
    json_t *outputs;

    outputs = parse("[{\"name\":\"DP-1\",\"enabled\":true,\"powered\":true,"
                     "\"usable_area\":{\"x\":0,\"y\":0,\"width\":1920,\"height\":1080}}]");
    umb_state_reset(&state);
    assert(umb_state_apply(&state, "outputs", outputs));
    json_decref(outputs);

    memset(&window, 0, sizeof(window));
    window.x = 100;
    window.y = 100;
    window.w = 200;
    window.h = 200;
    assert(!umb_state_window_offscreen(&state, &window));

    window.x = 5000;
    assert(umb_state_window_offscreen(&state, &window));

    window.x = -250;
    window.w = 400;
    assert(!umb_state_window_offscreen(&state, &window));
}

int main(void)
{
    test_windows();
    test_ie_modes();
    test_outputs();
    test_offscreen();
    puts("state tests passed");
    return 0;
}
