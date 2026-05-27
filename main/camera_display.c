#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "esp_log.h"
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

    int bpp = (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565) ? 2 : 3;
    size_t buf_size = BSP_LCD_H_RES * BSP_LCD_V_RES * bpp;

    // Pause LVGL so the buffers don't move under us while we snapshot.
    bsp_display_lock(portMAX_DELAY);

    // lcd_buffer[0] currently holds the most recently rendered GUI (LVGL has
    // been refreshing for a while by now). Mirror it into the other two LCD
    // framebuffers so all three share the same background. After we enable
    // dummy_draw, only the 320x320 camera region of each buffer will change.
    memcpy(lcd_buffer[1], lcd_buffer[0], buf_size);
    memcpy(lcd_buffer[2], lcd_buffer[0], buf_size);

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

    // Pick the largest centered square that's an integer multiple of the
    // output width so PPA's scale * block_h lands exactly on CAM_DISP_W.
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
        .out.buffer_size = ALIGN_UP(display_width * display_height *
                                        (APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? 2 : 3),
                                    data_cache_line_size),
        .out.pic_w = display_width,
        .out.pic_h = display_height,
        .out.block_offset_x = CAM_PHYS_X,
        .out.block_offset_y = CAM_PHYS_Y,
        .out.srm_cm = APP_VIDEO_FMT == APP_VIDEO_FMT_RGB565 ? PPA_SRM_COLOR_MODE_RGB565 : PPA_SRM_COLOR_MODE_RGB888,

        // Camera rotation: tweak this to change the displayed camera orientation.
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
