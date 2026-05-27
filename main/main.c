#include "esp_err.h"
#include "esp_log.h"
#include "esp_video_init.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "app_video.h"
#include "camera_display.h"

static const char *TAG = "app_main";

// ---- Web-style palette mirrored from python/gui.py ----
#define COLOR_BG_PAGE   0xF8F9FA
#define COLOR_TEXT      0x202124
#define COLOR_TEXT_SUB  0x5F6368
#define COLOR_CARD_BG   0xFFFFFF
#define COLOR_BORDER    0xDADCE0
#define COLOR_CLASS_VAL 0x1A73E8
#define COLOR_PROB_VAL  0x34A853
#define COLOR_SIGNATURE 0xBDBDBD

// ---- Layout in logical landscape coords (800 x 480) — matches python/gui.py ----
#define WIN_W           800
#define WIN_H           480
#define MARGIN_L        24
#define MARGIN_R        24
#define MARGIN_T        24
#define MARGIN_B        16
#define CONTENT_GAP     24

#define INFO_X          (CAM_LOGICAL_X + CAM_DISP_W + CONTENT_GAP)  // 24+320+24 = 368
#define INFO_Y          CAM_LOGICAL_Y                               // 80
#define INFO_W          (WIN_W - INFO_X - MARGIN_R)                 // 800-368-24 = 408
#define INFO_H          CAM_DISP_H                                  // 320

static lv_obj_t *s_class_val_label = NULL;
static lv_obj_t *s_prob_val_label = NULL;

static void build_gui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // -------- header --------
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Real-time Detection");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_pos(title, MARGIN_L, MARGIN_T);

    lv_obj_t *device = lv_label_create(scr);
    lv_label_set_text(device, "ESP32-P4");
    lv_obj_set_style_text_color(device, lv_color_hex(COLOR_TEXT_SUB), 0);
    lv_obj_set_style_text_font(device, &lv_font_montserrat_18, 0);
    lv_obj_align(device, LV_ALIGN_TOP_RIGHT, -MARGIN_R, MARGIN_T + 4);

    // -------- camera frame (LEFT, 320x320) --------
    // Dark placeholder; the camera stream lands on top of this region via PPA.
    lv_obj_t *cam_frame = lv_obj_create(scr);
    lv_obj_remove_style_all(cam_frame);
    lv_obj_set_size(cam_frame, CAM_DISP_W, CAM_DISP_H);
    lv_obj_set_pos(cam_frame, CAM_LOGICAL_X, CAM_LOGICAL_Y);
    lv_obj_set_style_bg_color(cam_frame, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(cam_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cam_frame, 0, 0);
    lv_obj_remove_flag(cam_frame, LV_OBJ_FLAG_SCROLLABLE);

    // -------- info card (RIGHT) --------
    lv_obj_t *info_frame = lv_obj_create(scr);
    lv_obj_remove_style_all(info_frame);
    lv_obj_set_size(info_frame, INFO_W, INFO_H);
    lv_obj_set_pos(info_frame, INFO_X, INFO_Y);
    lv_obj_set_style_bg_color(info_frame, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(info_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(info_frame, 16, 0);
    lv_obj_set_style_border_color(info_frame, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(info_frame, 1, 0);
    lv_obj_set_style_border_opa(info_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(info_frame, 24, 0);
    lv_obj_remove_flag(info_frame, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *info_title = lv_label_create(info_frame);
    lv_label_set_text(info_title, "Detection Result");
    lv_obj_set_style_text_color(info_title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(info_title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(info_title, 0, 0);

    lv_obj_t *class_hint = lv_label_create(info_frame);
    lv_label_set_text(class_hint, "Class");
    lv_obj_set_style_text_color(class_hint, lv_color_hex(COLOR_TEXT_SUB), 0);
    lv_obj_set_style_text_font(class_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(class_hint, 0, 48);

    s_class_val_label = lv_label_create(info_frame);
    lv_label_set_text(s_class_val_label, "Person");
    lv_obj_set_style_text_color(s_class_val_label, lv_color_hex(COLOR_CLASS_VAL), 0);
    lv_obj_set_style_text_font(s_class_val_label, &lv_font_montserrat_26, 0);
    lv_obj_set_pos(s_class_val_label, 0, 72);

    lv_obj_t *prob_hint = lv_label_create(info_frame);
    lv_label_set_text(prob_hint, "Probability");
    lv_obj_set_style_text_color(prob_hint, lv_color_hex(COLOR_TEXT_SUB), 0);
    lv_obj_set_style_text_font(prob_hint, &lv_font_montserrat_16, 0);
    lv_obj_set_pos(prob_hint, 0, 144);

    s_prob_val_label = lv_label_create(info_frame);
    lv_label_set_text(s_prob_val_label, "98.5%");
    lv_obj_set_style_text_color(s_prob_val_label, lv_color_hex(COLOR_PROB_VAL), 0);
    lv_obj_set_style_text_font(s_prob_val_label, &lv_font_montserrat_26, 0);
    lv_obj_set_pos(s_prob_val_label, 0, 168);

    // -------- footer signature --------
    lv_obj_t *signature = lv_label_create(scr);
    lv_label_set_text(signature, "------VERSER");
    lv_obj_set_style_text_color(signature, lv_color_hex(COLOR_SIGNATURE), 0);
    lv_obj_set_style_text_font(signature, &lv_font_montserrat_24, 0);
    lv_obj_align(signature, LV_ALIGN_BOTTOM_RIGHT, -MARGIN_R, -MARGIN_B);
    
    // lv_sysmon_hide_performance(NULL);

}

void app_main(void)
{
    // Landscape: native LCD is 480x800 portrait — rotate 90 CW so LVGL gives
    // us an 800x480 logical surface that matches python/gui.py.
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg  = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation        = ESP_LV_ADAPTER_ROTATE_90,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_TRIPLE_PARTIAL,
        .touch_flags = {
            .swap_xy  = 1,
            .mirror_x = 0,
            .mirror_y = 1,
        },
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    esp_lcd_panel_handle_t display_panel = bsp_display_get_panel_handle();
    i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();

    // 1. Build the GUI; LVGL refreshes it (rotated) into the LCD framebuffers.
    bsp_display_lock(portMAX_DELAY);
    build_gui();
    bsp_display_unlock();

    // 2. Wait for LVGL to fully render the rotated GUI into fb[0].
    vTaskDelay(pdMS_TO_TICKS(300));

    // 3. Snapshot fb[0] -> fb[1], fb[2], enable dummy_draw, prep PPA.
    camera_display_init(display_panel, disp);

    // 4. Bring up the camera.
    esp_err_t ret = app_video_main(i2c_bus);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "video main init failed with err=0x%x", ret);
        return;
    }

    int video_cam_fd0 = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0)
    {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }


    camera_display_alloc_and_set_bufs(video_cam_fd0);

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_display_frame_cb));
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));
}
