#pragma once

#include "esp_lcd_panel_ops.h"
#include "lvgl.h"
#include "app_video.h"

#ifdef __cplusplus
extern "C" {
#endif

// Camera window size.
#define CAM_DISP_W 320
#define CAM_DISP_H 320

// LOGICAL position of the 320x320 camera window in the 800x480 landscape GUI.
#define CAM_LOGICAL_X 24
#define CAM_LOGICAL_Y 80

// PHYSICAL position in the native 480x800 framebuffer after LVGL ROTATE_90.
#define CAM_PHYS_X (BSP_LCD_H_RES - CAM_LOGICAL_Y - CAM_DISP_H)
#define CAM_PHYS_Y (CAM_LOGICAL_X)

// Must be called AFTER the LVGL GUI has been rendered into the LCD framebuffers.
void camera_display_init(esp_lcd_panel_handle_t panel, lv_display_t *disp);

void camera_display_alloc_and_set_bufs(int video_fd);

void camera_display_frame_cb(
    uint8_t *camera_buf,
    uint8_t camera_buf_index,
    uint32_t camera_buf_hes,
    uint32_t camera_buf_ves,
    size_t camera_buf_len,
    void *user_data);

#ifdef __cplusplus
}
#endif
