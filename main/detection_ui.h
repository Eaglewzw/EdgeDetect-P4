#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// Number of horizontal bars rendered inside the info card. Detections beyond
// this count are dropped (caller should pre-sort by probability and pass the
// top ones first).
#define DETECTION_UI_MAX_BARS 5

// One detection result. `class_name` is held by reference — keep it alive
// until you next call detection_ui_update().
typedef struct {
    const char *class_name;
    float probability;        // 0.0 .. 1.0
} detection_t;

// Build the entire on-screen UI on the active screen: page background,
// header (title + device tag), the 320x320 camera frame placeholder, the
// detection info card with bars, and the footer signature. Call once at
// startup while holding the LVGL lock.
void detection_ui_build(void);

// Replace the visible bars with the given detections (truncated to
// DETECTION_UI_MAX_BARS). Unused rows are hidden. Safe to call from any
// task. NOTE: this only mutates LVGL state; because the host project
// freezes the framebuffer via dummy_draw, the change is only visible if
// the caller also arranges for the updated widgets to be re-rendered
// (snapshot refresh or per-row stamp blit).
void detection_ui_update(const detection_t *detections, int count);

#ifdef __cplusplus
}
#endif
