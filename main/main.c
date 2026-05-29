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
#include "detection_ui.h"

static const char *TAG = "app_main";

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

    // 1. Build the entire on-screen UI; LVGL refreshes it (rotated) into the
    //    LCD framebuffers over the next few ticks.
    bsp_display_lock(portMAX_DELAY);
    detection_ui_build();
    bsp_display_unlock();

    // 2. Wait for LVGL to fully render the rotated GUI into a framebuffer.
    vTaskDelay(pdMS_TO_TICKS(300));

    // 3. Snapshot the most-complete fb into all 3 lcd_buffers, enable
    //    dummy_draw, and prep PPA for the camera path.
    camera_display_init(display_panel, disp);

    // 4. Bring up the camera.
    esp_err_t ret = app_video_main(i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "video main init failed with err=0x%x", ret);
        return;
    }

    int video_cam_fd0 = app_video_open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, APP_VIDEO_FMT);
    if (video_cam_fd0 < 0) {
        ESP_LOGE(TAG, "video cam open failed");
        return;
    }

    camera_display_alloc_and_set_bufs(video_cam_fd0);

    ESP_ERROR_CHECK(app_video_register_frame_operation_cb(camera_display_frame_cb));
    ESP_ERROR_CHECK(app_video_stream_task_start(video_cam_fd0, 0, NULL));
}
