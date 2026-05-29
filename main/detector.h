#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Camera input size handed to the network (matches the ONNX input shape).
#define DETECTOR_INPUT_W 320
#define DETECTOR_INPUT_H 320
#define DETECTOR_INPUT_CHANNELS 3

// Upper bound on how many post-NMS detections the host code can consume per
// frame. Detector internally may produce more; only the highest-confidence
// `DETECTOR_MAX_BOXES` are kept.
#define DETECTOR_MAX_BOXES 5

// One result. All coordinates are normalised to [0, 1] in the network's
// input frame (i.e. the 320x320 image that detector_submit_rgb565 received).
typedef struct {
    float x_center;
    float y_center;
    float w;
    float h;
    float confidence;     // 0..1, after NMS
} detection_box_t;

// Initialise the model from the embedded .espdl, allocate input/output
// tensors and SPIRAM scratch buffers, and start the inference task pinned
// to Core 1. Must be called from app_main before any submit/get call.
esp_err_t detector_init(void);

// Submit a fresh camera frame for inference. Non-blocking and lock-free
// for the caller: if the inference task is still working on the previous
// frame, this call drops the new one. The buffer is read synchronously
// inside this call (it's safe for the caller to free / reuse it after
// the function returns).
//
// Format: tightly-packed 320*320 RGB565 (2 bytes/px), upright (NOT mirrored).
void detector_submit_rgb565(const void *rgb565_320x320);

// Snapshot the most recent post-NMS detections into `out` and return how
// many were copied (0 .. min(max_boxes, DETECTOR_MAX_BOXES)). Safe to call
// from any task; the inference task takes a brief mutex while writing.
int detector_get_latest(detection_box_t *out, int max_boxes);

#ifdef __cplusplus
}
#endif
