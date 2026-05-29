// Detection inference: loads car_mcu.espdl via ESP-DL, runs the
// quantised model on each camera frame submitted, decodes the [6300, 5]
// output into bounding boxes, and stores the post-NMS results for the
// camera display path to read.
//
// Threading model
// ---------------
// * detector_submit_rgb565()  — called from the camera frame callback on
//   Core 0. Copies the frame into an SPIRAM staging buffer and notifies
//   the inference task. Drops the frame if the previous one is still in
//   the staging slot.
// * inference_task            — pinned to Core 1. Pops the staging slot,
//   feeds the model input tensor, runs forward, decodes detections,
//   NMS-filters them, and stores the top boxes under a mutex.
// * detector_get_latest()     — called from anywhere. Briefly takes the
//   results mutex and copies out the latest box list.
//
// This keeps Core 0 free for the LVGL/PPA display path; only the small
// memcpy and a semaphore wakeup happen on the camera task.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "dl_model_base.hpp"
#include "dl_tensor_base.hpp"

#include "detector.h"

static const char *TAG = "detector";

// ---- Linker symbols for the embedded repacked .espdl ----
extern "C" const uint8_t car_mcu_packed_espdl_start[] asm("_binary_car_mcu_packed_espdl_start");

// ---- Post-processing tunables ----
static constexpr float CONF_THRESHOLD = 0.45f;
static constexpr float NMS_IOU_THRESHOLD = 0.45f;
static constexpr int   MODEL_ANCHORS = 6300;       // [6300, 5] head
static constexpr int   MODEL_STRIDE  = 5;          // [cx, cy, w, h, conf]

// ---- Module state ----
namespace {

dl::Model *s_model = nullptr;
dl::TensorBase *s_input_tensor = nullptr;
dl::TensorBase *s_output_tensor = nullptr;

// Staging slot for the latest submitted frame.
uint8_t *s_pending_rgb565 = nullptr;
SemaphoreHandle_t s_pending_lock   = nullptr;   // protects s_pending_rgb565 + flag
SemaphoreHandle_t s_frame_ready    = nullptr;   // binary, signalled on new frame
volatile bool s_pending_filled = false;

// Inference task's private working buffer (so it can release the staging
// lock immediately and not stall the camera path during forward).
uint8_t *s_work_rgb565 = nullptr;

// Output of the most recent inference, guarded by s_results_lock.
detection_box_t s_results[DETECTOR_MAX_BOXES];
int s_results_count = 0;
SemaphoreHandle_t s_results_lock = nullptr;

constexpr size_t INPUT_PIXELS = (size_t)DETECTOR_INPUT_W * DETECTOR_INPUT_H;
constexpr size_t INPUT_RGB565_BYTES = INPUT_PIXELS * 2;

}  // namespace

// =========================================================
// RGB565 -> model input (NCHW int8 quantised)
// =========================================================
//
// The model was quantised from a 3x320x320 fp32 input with [0, 1] scaling.
// ESP-DL stores the input tensor as int8 with a per-tensor scale & zero
// point — we let the tensor's quantisation parameters drive the conversion.
//
// RGB565 layout: 2 bytes per pixel, RRRRRGGG GGGBBBBB (little-endian on P4
// → low byte = GGGBBBBB, high byte = RRRRRGGG). We unpack to 8-bit per
// channel, normalise to [0, 1], then quantise to the tensor's int8 scale.
static void rgb565_to_input_tensor(const uint8_t *rgb565)
{
    if (!s_input_tensor) return;

    auto *dst = static_cast<int8_t *>(s_input_tensor->get_element_ptr());
    // ESP-DL stores quantisation as a power-of-two exponent E: real = q * 2^E.
    // Input/output tensors are per-tensor, so .get() returns the single E.
    const int input_exp = s_input_tensor->exponent.get();
    const float scale = std::ldexp(1.0f, input_exp);
    // Symmetric int8 (zp = 0).
    auto quantise = [&](float v) -> int8_t {
        float q = v / scale;
        if (q > 127.0f)  q = 127.0f;
        if (q < -128.0f) q = -128.0f;
        return static_cast<int8_t>(std::lrintf(q));
    };

    // Output is planar (channel-major): [C=3][H=320][W=320].
    const size_t plane = INPUT_PIXELS;
    for (size_t i = 0; i < INPUT_PIXELS; i++) {
        uint16_t px = (uint16_t)rgb565[i * 2] | ((uint16_t)rgb565[i * 2 + 1] << 8);
        // 5-6-5 -> 8-8-8 via shift+replicate
        uint8_t r8 = ((px >> 11) & 0x1F) << 3; r8 |= r8 >> 5;
        uint8_t g8 = ((px >> 5)  & 0x3F) << 2; g8 |= g8 >> 6;
        uint8_t b8 =  (px        & 0x1F) << 3; b8 |= b8 >> 5;

        dst[0 * plane + i] = quantise(r8 / 255.0f);
        dst[1 * plane + i] = quantise(g8 / 255.0f);
        dst[2 * plane + i] = quantise(b8 / 255.0f);
    }
}

// =========================================================
// Output decode + NMS
// =========================================================
struct RawBox {
    float cx, cy, w, h, conf;
};

static float iou(const RawBox &a, const RawBox &b)
{
    float ax1 = a.cx - a.w * 0.5f, ay1 = a.cy - a.h * 0.5f;
    float ax2 = a.cx + a.w * 0.5f, ay2 = a.cy + a.h * 0.5f;
    float bx1 = b.cx - b.w * 0.5f, by1 = b.cy - b.h * 0.5f;
    float bx2 = b.cx + b.w * 0.5f, by2 = b.cy + b.h * 0.5f;

    float ix1 = std::max(ax1, bx1), iy1 = std::max(ay1, by1);
    float ix2 = std::min(ax2, bx2), iy2 = std::min(ay2, by2);
    float iw  = std::max(0.0f, ix2 - ix1);
    float ih  = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    float area_a = (ax2 - ax1) * (ay2 - ay1);
    float area_b = (bx2 - bx1) * (by2 - by1);
    float uni = area_a + area_b - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

static int decode_and_nms(detection_box_t *out_boxes)
{
    if (!s_output_tensor) return 0;

    const size_t numel = s_output_tensor->get_size();
    if (numel < (size_t)MODEL_ANCHORS * MODEL_STRIDE) {
        ESP_LOGW(TAG, "unexpected output size %zu", numel);
        return 0;
    }

    // Output is int8 quantised; dequantise with the tensor's per-tensor
    // exponent (real = q * 2^E).
    const int out_exp = s_output_tensor->exponent.get();
    const float out_scale = std::ldexp(1.0f, out_exp);
    const auto *src = static_cast<const int8_t *>(s_output_tensor->get_element_ptr());

    std::vector<RawBox> kept;
    kept.reserve(64);

    // The network's output is in network input pixel coords (0..320). We
    // normalise on the fly to [0, 1].
    constexpr float NORM = 1.0f / (float)DETECTOR_INPUT_W;
    for (int i = 0; i < MODEL_ANCHORS; i++) {
        float conf = src[i * MODEL_STRIDE + 4] * out_scale;
        if (conf < CONF_THRESHOLD) continue;

        RawBox b;
        b.cx   = src[i * MODEL_STRIDE + 0] * out_scale * NORM;
        b.cy   = src[i * MODEL_STRIDE + 1] * out_scale * NORM;
        b.w    = src[i * MODEL_STRIDE + 2] * out_scale * NORM;
        b.h    = src[i * MODEL_STRIDE + 3] * out_scale * NORM;
        b.conf = conf;
        kept.push_back(b);
    }

    // Sort by confidence descending and run a basic greedy NMS.
    std::sort(kept.begin(), kept.end(),
              [](const RawBox &a, const RawBox &b) { return a.conf > b.conf; });

    std::vector<bool> suppressed(kept.size(), false);
    int out_n = 0;
    for (size_t i = 0; i < kept.size() && out_n < DETECTOR_MAX_BOXES; i++) {
        if (suppressed[i]) continue;
        out_boxes[out_n].x_center  = kept[i].cx;
        out_boxes[out_n].y_center  = kept[i].cy;
        out_boxes[out_n].w         = kept[i].w;
        out_boxes[out_n].h         = kept[i].h;
        out_boxes[out_n].confidence = kept[i].conf;
        out_n++;
        for (size_t j = i + 1; j < kept.size(); j++) {
            if (!suppressed[j] && iou(kept[i], kept[j]) > NMS_IOU_THRESHOLD) {
                suppressed[j] = true;
            }
        }
    }
    return out_n;
}

// =========================================================
// Inference task
// =========================================================
static void inference_task(void * /*arg*/)
{
    ESP_LOGI(TAG, "inference task on core %d", xPortGetCoreID());

    detection_box_t local_boxes[DETECTOR_MAX_BOXES];

    while (true) {
        if (xSemaphoreTake(s_frame_ready, portMAX_DELAY) != pdTRUE) continue;

        // 1. Lift the pending frame into our private working buffer so the
        //    submit path can refill the slot during forward.
        xSemaphoreTake(s_pending_lock, portMAX_DELAY);
        if (!s_pending_filled) {
            xSemaphoreGive(s_pending_lock);
            continue;
        }
        std::memcpy(s_work_rgb565, s_pending_rgb565, INPUT_RGB565_BYTES);
        s_pending_filled = false;
        xSemaphoreGive(s_pending_lock);

        // 2. Quantise into the model input tensor.
        int64_t t0 = esp_timer_get_time();
        rgb565_to_input_tensor(s_work_rgb565);
        int64_t t1 = esp_timer_get_time();

        // 3. Forward.
        s_model->run(dl::RUNTIME_MODE_AUTO);
        int64_t t2 = esp_timer_get_time();

        // 4. Decode + NMS.
        int n = decode_and_nms(local_boxes);
        int64_t t3 = esp_timer_get_time();

        // 5. Publish.
        xSemaphoreTake(s_results_lock, portMAX_DELAY);
        std::memcpy(s_results, local_boxes, sizeof(local_boxes));
        s_results_count = n;
        xSemaphoreGive(s_results_lock);

        // Light heartbeat. Comment out once stable.
        static int log_div = 0;
        if (++log_div >= 15) {
            log_div = 0;
            ESP_LOGI(TAG, "n=%d  pre=%dms  fwd=%dms  post=%dms",
                     n, (int)((t1 - t0) / 1000),
                        (int)((t2 - t1) / 1000),
                        (int)((t3 - t2) / 1000));
        }
    }
}

// =========================================================
// Public API
// =========================================================
extern "C" esp_err_t detector_init(void)
{
    if (s_model) return ESP_OK;

    ESP_LOGI(TAG, "loading model from rodata @ %p",
             car_mcu_packed_espdl_start);

    // Repacked multi-model container: model_num=1, index, name, sub-header.
    // RODATA mode treats the first arg as a raw data pointer. ESP-DL reads
    // the EDL2 wrapper and navigates the index to find the actual model.
    s_model = new dl::Model(
        reinterpret_cast<const char *>(car_mcu_packed_espdl_start),
        fbs::MODEL_LOCATION_IN_FLASH_RODATA);
    if (!s_model) {
        ESP_LOGE(TAG, "dl::Model alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // ESP-DL >= 3.3 exposes inputs/outputs as name->TensorBase maps. The
    // ONNX names came through verbatim: "images" / "output".
    auto inputs  = s_model->get_inputs();
    auto outputs = s_model->get_outputs();
    if (inputs.empty() || outputs.empty()) {
        ESP_LOGE(TAG, "model has no inputs/outputs");
        return ESP_FAIL;
    }
    s_input_tensor  = inputs.begin()->second;
    s_output_tensor = outputs.begin()->second;

    ESP_LOGI(TAG, "input  %s: numel=%zu  exponent=%d  per_ch=%d",
             inputs.begin()->first.c_str(),
             s_input_tensor->get_size(),
             s_input_tensor->exponent.get(),
             (int)s_input_tensor->exponent.is_per_channel());
    ESP_LOGI(TAG, "output %s: numel=%zu  exponent=%d  per_ch=%d",
             outputs.begin()->first.c_str(),
             s_output_tensor->get_size(),
             s_output_tensor->exponent.get(),
             (int)s_output_tensor->exponent.is_per_channel());

    // Buffers.
    s_pending_rgb565 = (uint8_t *)heap_caps_malloc(INPUT_RGB565_BYTES,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_work_rgb565    = (uint8_t *)heap_caps_malloc(INPUT_RGB565_BYTES,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pending_rgb565 || !s_work_rgb565) {
        ESP_LOGE(TAG, "RGB565 buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_pending_lock = xSemaphoreCreateMutex();
    s_results_lock = xSemaphoreCreateMutex();
    s_frame_ready  = xSemaphoreCreateBinary();
    if (!s_pending_lock || !s_results_lock || !s_frame_ready) {
        ESP_LOGE(TAG, "sync primitive alloc failed");
        return ESP_ERR_NO_MEM;
    }

    // Stack size: ESP-DL ops + std::vector temporaries fit comfortably in 16k.
    BaseType_t ok = xTaskCreatePinnedToCore(
        inference_task, "detector", 16384, nullptr, 4, nullptr,
        /*core=*/1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "inference task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

extern "C" void detector_submit_rgb565(const void *rgb565_320x320)
{
    if (!s_pending_rgb565) return;
    // Lock briefly to avoid stomping the slot while inference_task lifts it.
    if (xSemaphoreTake(s_pending_lock, 0) != pdTRUE) return;
    std::memcpy(s_pending_rgb565, rgb565_320x320, INPUT_RGB565_BYTES);
    s_pending_filled = true;
    xSemaphoreGive(s_pending_lock);
    xSemaphoreGive(s_frame_ready);
}

extern "C" int detector_get_latest(detection_box_t *out, int max_boxes)
{
    if (!out || max_boxes <= 0 || !s_results_lock) return 0;
    int n = 0;
    if (xSemaphoreTake(s_results_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
        n = s_results_count < max_boxes ? s_results_count : max_boxes;
        std::memcpy(out, s_results, n * sizeof(detection_box_t));
        xSemaphoreGive(s_results_lock);
    }
    return n;
}
