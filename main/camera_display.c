#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_private/esp_cache_private.h"
#include "driver/ppa.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "camera_display.h"

#define ALIGN_UP(num, align) (((num) + ((align) - 1)) & ~((align) - 1))

static const char *TAG = "camera_display";

static ppa_client_handle_t ppa_srm_handle = NULL;
static size_t data_cache_line_size = 0;
static void *lcd_buffer[CONFIG_BSP_LCD_DPI_BUFFER_NUMS];
static lv_display_t *s_disp = NULL;

#define LCD_BPP ((APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565) ? 2 : 3)

// =============================================================
// Self-rendered "FPS" stamp (replaces LVGL's perf monitor)
// =============================================================
// We disabled LVGL's built-in perf monitor because dummy_draw freezes its
// updates. Instead we count actual camera frames in the V4L2 callback, render
// the resulting FPS string into a SPIRAM buffer via lv_draw_label on a raw
// lv_layer_t (no widget tree mutation), and PPA-blit that stamp into the
// LCD framebuffer on every camera frame.
//
// Position: bottom-left of the 800x480 landscape view (where the LVGL perf
// monitor used to sit when CONFIG_LV_PERF_MONITOR_ALIGN_BOTTOM_LEFT was on).

#define FPS_STAMP_W      220
#define FPS_STAMP_H      32
#define FPS_LOGICAL_X    8
#define FPS_LOGICAL_Y    (480 - FPS_STAMP_H - 8)   // 440
#define FPS_UPDATE_US    500000                    // re-render every 500 ms

static void *s_fps_stamp_buf = NULL;
static size_t s_fps_stamp_buf_size = 0;
static int s_fps_phys_x = 0;
static int s_fps_phys_y = 0;
static lv_draw_buf_t s_stamp_draw_buf;       // draw_buf header pointing at the stamp
static SemaphoreHandle_t s_stamp_mutex = NULL;

static uint32_t s_frame_count = 0;
static int64_t s_last_fps_update_us = 0;

// Map a logical top-left to physical fb top-left under LVGL ROTATE_90 (90 CW).
static inline void logical_to_phys_topleft(int lx, int ly, int lh,
                                           int *px, int *py)
{
    *px = BSP_LCD_H_RES - 1 - (ly + lh - 1);
    *py = lx;
}

static void render_fps_stamp(const char *text)
{
    bsp_display_lock(portMAX_DELAY);

    // Fill background ourselves (RGB565 conversion of 0xF8F9FA, the page bg).
    uint16_t bg = lv_color_to_u16(lv_color_hex(0xF8F9FA));
    uint16_t *p = (uint16_t *)s_fps_stamp_buf;
    for (int i = 0; i < FPS_STAMP_W * FPS_STAMP_H; i++) {
        p[i] = bg;
    }

    // Standalone draw layer pointing at our stamp buffer. No LVGL widget
    // tree mutation, so the active screen stays clean and dummy_draw can
    // freeze the snapshot reliably.
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = &s_stamp_draw_buf;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer.buf_area = (lv_area_t){0, 0, FPS_STAMP_W - 1, FPS_STAMP_H - 1};
    layer._clip_area = layer.buf_area;
    layer.phy_clip_area = layer.buf_area;

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text = text;
    dsc.font = &lv_font_montserrat_16;
    dsc.color = lv_color_hex(0x5F6368);   // subdued gray, matches sub-text style
    dsc.align = LV_TEXT_ALIGN_LEFT;

    lv_area_t area = layer.buf_area;
    lv_draw_label(&layer, &dsc, &area);

    // Drive the draw tasks to completion (same loop the LVGL canvas uses).
    while (layer.draw_task_head) {
        lv_draw_dispatch_wait_for_request();
        bool task_dispatched = lv_draw_dispatch_layer(s_disp, &layer);
        if (!task_dispatched) {
            lv_draw_wait_for_finish();
            lv_draw_dispatch_request();
        }
    }

    bsp_display_unlock();

    // Flush CPU cache so the PPA DMA reads the freshly drawn pixels.
    esp_cache_msync(s_fps_stamp_buf, s_fps_stamp_buf_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M);
}

static void blit_fps_stamp(void *lcd_buf)
{
    ppa_srm_oper_config_t cfg = {
        .in.buffer = s_fps_stamp_buf,
        .in.pic_w = FPS_STAMP_W,
        .in.pic_h = FPS_STAMP_H,
        .in.block_w = FPS_STAMP_W,
        .in.block_h = FPS_STAMP_H,
        .in.block_offset_x = 0,
        .in.block_offset_y = 0,
        .in.srm_cm = PPA_SRM_COLOR_MODE_RGB565,

        .out.buffer = lcd_buf,
        .out.buffer_size = ALIGN_UP(BSP_LCD_H_RES * BSP_LCD_V_RES * LCD_BPP,
                                    data_cache_line_size),
        .out.pic_w = BSP_LCD_H_RES,
        .out.pic_h = BSP_LCD_V_RES,
        .out.block_offset_x = s_fps_phys_x,
        .out.block_offset_y = s_fps_phys_y,
        .out.srm_cm = PPA_SRM_COLOR_MODE_RGB565,

        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,  // matches GUI orientation
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = 0,
        .mirror_y = 0,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "FPS stamp PPA blit failed: %d", ret);
    }
}

static void fps_stamp_init(void)
{
    logical_to_phys_topleft(FPS_LOGICAL_X, FPS_LOGICAL_Y, FPS_STAMP_H,
                            &s_fps_phys_x, &s_fps_phys_y);

    s_fps_stamp_buf_size = ALIGN_UP(FPS_STAMP_W * FPS_STAMP_H * 2 /*RGB565*/,
                                    data_cache_line_size);
    s_fps_stamp_buf = heap_caps_aligned_calloc(data_cache_line_size, 1,
                                               s_fps_stamp_buf_size,
                                               MALLOC_CAP_SPIRAM);
    assert(s_fps_stamp_buf);

    // Initialise a draw_buf header pointing at our stamp memory. No widget
    // is added to the screen tree.
    uint32_t stride = lv_draw_buf_width_to_stride(FPS_STAMP_W, LV_COLOR_FORMAT_RGB565);
    lv_draw_buf_init(&s_stamp_draw_buf, FPS_STAMP_W, FPS_STAMP_H,
                     LV_COLOR_FORMAT_RGB565, stride,
                     s_fps_stamp_buf, stride * FPS_STAMP_H);

    s_stamp_mutex = xSemaphoreCreateMutex();
    assert(s_stamp_mutex);

    s_last_fps_update_us = esp_timer_get_time();
    // NOTE: skip initial render_fps_stamp() here. Calling LVGL's draw
    // dispatcher while we hold the display lock and just before enabling
    // dummy_draw seems to leave the framebuffer in an unexpected state.
    // First real render happens in frame_cb after 500 ms, which is well
    // after dummy_draw is fully active.
}

void camera_display_init(esp_lcd_panel_handle_t panel, lv_display_t *disp)
{
    s_disp = disp;

    ppa_client_config_t ppa_srm_config = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_srm_config, &ppa_srm_handle));

    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &data_cache_line_size));

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(
        panel, 3,
        &lcd_buffer[0], &lcd_buffer[1], &lcd_buffer[2]));

    size_t buf_size = BSP_LCD_H_RES * BSP_LCD_V_RES * LCD_BPP;

    bsp_display_lock(portMAX_DELAY);

    // The 3 LCD framebuffers don't end up in sync — under LVGL ROTATE_90 the
    // adapter only writes to whichever fb is currently the back buffer, so
    // the other two can still be zeroed. Pick the one with the most rendered
    // content as the snapshot source.
    //
    // We probe a few well-spread locations in the native 480x800 layout:
    // every rendered GUI pixel is non-zero (page bg #F8F9FA, info card,
    // camera placeholder, etc.). The fb with the highest non-zero score wins.
    const size_t probe_offsets[] = {
        (BSP_LCD_V_RES / 4) * BSP_LCD_H_RES + (BSP_LCD_H_RES / 4),
        (BSP_LCD_V_RES / 2) * BSP_LCD_H_RES + (BSP_LCD_H_RES / 2),
        (3 * BSP_LCD_V_RES / 4) * BSP_LCD_H_RES + (3 * BSP_LCD_H_RES / 4),
        (BSP_LCD_V_RES - 4) * BSP_LCD_H_RES + 4,
    };
    const int n_probes = sizeof(probe_offsets) / sizeof(probe_offsets[0]);

    // Scoring: each non-zero probe pixel scores 10, plus a +1 bonus per
    // additional *distinct* colour. A fb showing only the page background
    // scores 10*4 + 1 (one distinct colour) = 41; a fb that's also got
    // widgets drawn (multiple colours) scores higher and wins.
    int best_idx = 0;
    int best_score = -1;
    for (int i = 0; i < 3; i++) {
        uint16_t *p = (uint16_t *)lcd_buffer[i];
        int nonzero = 0;
        int distinct = 0;
        uint16_t seen[4] = {0};
        for (int k = 0; k < n_probes; k++) {
            uint16_t v = p[probe_offsets[k]];
            if (v != 0x0000) nonzero++;
            bool already = false;
            for (int s = 0; s < distinct; s++) {
                if (seen[s] == v) { already = true; break; }
            }
            if (!already) {
                seen[distinct++] = v;
            }
        }
        int score = nonzero * 10 + distinct;
        ESP_LOGI(TAG, "snapshot scan: lcd_buffer[%d] nonzero=%d/%d distinct=%d score=%d  (samples %04x %04x %04x %04x)",
                 i, nonzero, n_probes, distinct, score,
                 p[probe_offsets[0]], p[probe_offsets[1]],
                 p[probe_offsets[2]], p[probe_offsets[3]]);
        if (score > best_score) {
            best_score = score;
            best_idx = i;
        }
    }
    ESP_LOGI(TAG, "snapshot source: lcd_buffer[%d]", best_idx);

    void *src_fb = lcd_buffer[best_idx];

    // Mirror the chosen fb into the other two so all 3 share the GUI.
    for (int i = 0; i < 3; i++) {
        if (lcd_buffer[i] != src_fb) {
            memcpy(lcd_buffer[i], src_fb, buf_size);
        }
    }

    // Build the FPS stamp infrastructure while LVGL is still locked.
    fps_stamp_init();

    ESP_ERROR_CHECK(esp_lv_adapter_set_dummy_draw(disp, true));

    bsp_display_unlock();
}

void camera_display_alloc_and_set_bufs(int video_fd)
{
    void *camera_buf[2];
    for (int i = 0; i < 2; i++)
    {
        camera_buf[i] = heap_caps_aligned_calloc(
            data_cache_line_size,
            1,
            app_video_get_buf_size(),
            MALLOC_CAP_SPIRAM);
    }
    ESP_ERROR_CHECK(app_video_set_bufs(video_fd, 2, (void *)camera_buf));
}

void camera_display_frame_cb(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data)
{
    static bool printed_dims = false;
    if (!printed_dims) {
        ESP_LOGI(TAG, "first frame: camera_buf_hes=%" PRIu32 " camera_buf_ves=%" PRIu32,
                 camera_buf_hes, camera_buf_ves);
        printed_dims = true;
    }

    const uint32_t display_width = BSP_LCD_H_RES;
    const uint32_t display_height = BSP_LCD_V_RES;

    // ---- 1. Camera image ----
    uint32_t max_square = (camera_buf_hes < camera_buf_ves) ? camera_buf_hes : camera_buf_ves;
    uint32_t crop_size = (max_square / CAM_DISP_W) * CAM_DISP_W;
    if (crop_size == 0) crop_size = CAM_DISP_W;
    uint32_t in_offset_x = (camera_buf_hes - crop_size) / 2;
    uint32_t in_offset_y = (camera_buf_ves - crop_size) / 2;
    float scale = (float)CAM_DISP_W / (float)crop_size;

    ppa_srm_oper_config_t srm_config = {
        .in.buffer = camera_buf,
        .in.pic_w = camera_buf_hes,
        .in.pic_h = camera_buf_ves,
        .in.block_w = crop_size,
        .in.block_h = crop_size,
        .in.block_offset_x = in_offset_x,
        .in.block_offset_y = in_offset_y,
        .in.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .out.buffer = lcd_buffer[camera_buf_index],
        .out.buffer_size = ALIGN_UP(display_width * display_height * LCD_BPP,
                                    data_cache_line_size),
        .out.pic_w = display_width,
        .out.pic_h = display_height,
        .out.block_offset_x = CAM_PHYS_X,
        .out.block_offset_y = CAM_PHYS_Y,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        .rotation_angle = PPA_SRM_ROTATION_ANGLE_0,
        .scale_x = scale,
        .scale_y = scale,
        .mirror_x = 0,
        .mirror_y = 1,
        .rgb_swap = 0,
        .byte_swap = 0,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };

    esp_err_t ret = ppa_do_scale_rotate_mirror(ppa_srm_handle, &srm_config);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "PPA SRM failed: %d", ret);
        return;
    }

    // ---- 2. FPS counter ----
    // Count this frame; every FPS_UPDATE_US re-render the stamp with the
    // newly computed rate. Per-frame work is just the PPA blit below.
    s_frame_count++;
    int64_t now_us = esp_timer_get_time();
    int64_t elapsed_us = now_us - s_last_fps_update_us;
    if (elapsed_us >= FPS_UPDATE_US) {
        float fps = (float)s_frame_count * 1e6f / (float)elapsed_us;
        s_frame_count = 0;
        s_last_fps_update_us = now_us;

        char buf[32];
        snprintf(buf, sizeof(buf), "FPS: %.4f", fps);

        if (xSemaphoreTake(s_stamp_mutex, 0) == pdTRUE) {
            render_fps_stamp(buf);
            xSemaphoreGive(s_stamp_mutex);
        }
    }

    if (s_stamp_mutex && xSemaphoreTake(s_stamp_mutex, pdMS_TO_TICKS(2)) == pdTRUE) {
        blit_fps_stamp(lcd_buffer[camera_buf_index]);
        xSemaphoreGive(s_stamp_mutex);
    }

    // ---- 3. Push to LCD ----
    ret = esp_lv_adapter_dummy_draw_blit(
        s_disp,
        0, 0,
        display_width,
        display_height,
        lcd_buffer[camera_buf_index],
        true);

    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Dummy draw blit failed: %d", ret);
    }
}
