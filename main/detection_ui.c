#include <stdio.h>
#include "lvgl.h"
#include "detection_ui.h"
#include "camera_display.h"   // CAM_LOGICAL_X/Y, CAM_DISP_W/H

// ============================================================
// Web-style palette mirrored from python/gui.py
// ============================================================
#define COLOR_BG_PAGE   0xF8F9FA
#define COLOR_TEXT      0x202124
#define COLOR_TEXT_SUB  0x5F6368
#define COLOR_CARD_BG   0xFFFFFF
#define COLOR_BORDER    0xDADCE0
#define COLOR_BAR_TRACK 0xE8EAED
#define COLOR_BAR_FILL  0x1A73E8
#define COLOR_SIGNATURE 0xBDBDBD

// ============================================================
// Layout (LVGL logical landscape coords, 800 x 480)
// ============================================================
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

// Card internal layout.
#define CARD_PAD       24
#define TITLE_H        28
#define TITLE_GAP      16
#define ROW_H          32
#define ROW_GAP        8
#define NAME_W         96
#define NAME_GAP       8
#define PCT_W          60
#define PCT_GAP        8
#define BAR_H          8

// ============================================================
// Internal state
// ============================================================
typedef struct {
    lv_obj_t *row;
    lv_obj_t *name_label;
    lv_obj_t *bar;
    lv_obj_t *pct_label;
} bar_row_t;

static lv_obj_t *s_card = NULL;
static bar_row_t s_bars[DETECTION_UI_MAX_BARS];

// ============================================================
// Sub-builders
// ============================================================
static void build_header(lv_obj_t *scr)
{
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
}

static void build_camera_frame(lv_obj_t *scr)
{
    // Dark placeholder; the camera stream lands on top of this region via PPA.
    lv_obj_t *cam_frame = lv_obj_create(scr);
    lv_obj_remove_style_all(cam_frame);
    lv_obj_set_size(cam_frame, CAM_DISP_W, CAM_DISP_H);
    lv_obj_set_pos(cam_frame, CAM_LOGICAL_X, CAM_LOGICAL_Y);
    lv_obj_set_style_bg_color(cam_frame, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_bg_opa(cam_frame, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cam_frame, 0, 0);
    lv_obj_remove_flag(cam_frame, LV_OBJ_FLAG_SCROLLABLE);
}

static void build_footer(lv_obj_t *scr)
{
    lv_obj_t *signature = lv_label_create(scr);
    lv_label_set_text(signature, "------VERSER");
    lv_obj_set_style_text_color(signature, lv_color_hex(COLOR_SIGNATURE), 0);
    lv_obj_set_style_text_font(signature, &lv_font_montserrat_24, 0);
    lv_obj_align(signature, LV_ALIGN_BOTTOM_RIGHT, -MARGIN_R, -MARGIN_B);
}

static void create_bar_row(lv_obj_t *parent, int idx, int row_y, int content_w)
{
    bar_row_t *r = &s_bars[idx];

    r->row = lv_obj_create(parent);
    lv_obj_remove_style_all(r->row);
    lv_obj_set_size(r->row, content_w, ROW_H);
    lv_obj_set_pos(r->row, 0, row_y);
    lv_obj_remove_flag(r->row, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Class name (left) ----
    r->name_label = lv_label_create(r->row);
    lv_obj_set_size(r->name_label, NAME_W, ROW_H);
    lv_obj_set_pos(r->name_label, 0, 0);
    lv_obj_set_style_text_color(r->name_label, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(r->name_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(r->name_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(r->name_label, (ROW_H - 18) / 2, 0);
    lv_label_set_text(r->name_label, "");

    // ---- Progress bar (middle) ----
    int bar_x = NAME_W + NAME_GAP;
    int bar_w = content_w - NAME_W - NAME_GAP - PCT_W - PCT_GAP;
    r->bar = lv_bar_create(r->row);
    lv_obj_remove_style_all(r->bar);
    lv_obj_set_size(r->bar, bar_w, BAR_H);
    lv_obj_set_pos(r->bar, bar_x, (ROW_H - BAR_H) / 2);
    // Track
    lv_obj_set_style_bg_color(r->bar, lv_color_hex(COLOR_BAR_TRACK), 0);
    lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r->bar, BAR_H / 2, 0);
    // Indicator
    lv_obj_set_style_bg_color(r->bar, lv_color_hex(COLOR_BAR_FILL), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r->bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_bar_set_range(r->bar, 0, 1000);   // permille for one-decimal precision
    lv_bar_set_value(r->bar, 0, LV_ANIM_OFF);

    // ---- Percentage (right) ----
    int pct_x = bar_x + bar_w + PCT_GAP;
    r->pct_label = lv_label_create(r->row);
    lv_obj_set_size(r->pct_label, PCT_W, ROW_H);
    lv_obj_set_pos(r->pct_label, pct_x, 0);
    lv_obj_set_style_text_color(r->pct_label, lv_color_hex(COLOR_TEXT_SUB), 0);
    lv_obj_set_style_text_font(r->pct_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(r->pct_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_pad_top(r->pct_label, (ROW_H - 18) / 2, 0);
    lv_label_set_text(r->pct_label, "");

    // Empty by default — revealed when detection_ui_update() fills it.
    lv_obj_add_flag(r->row, LV_OBJ_FLAG_HIDDEN);
}

static void build_info_card(lv_obj_t *scr)
{
    s_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, INFO_W, INFO_H);
    lv_obj_set_pos(s_card, INFO_X, INFO_Y);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(COLOR_CARD_BG), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 16, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(COLOR_BORDER), 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_card, CARD_PAD, 0);
    lv_obj_remove_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    // Title
    lv_obj_t *title = lv_label_create(s_card);
    lv_label_set_text(title, "Detection Result");
    lv_obj_set_style_text_color(title, lv_color_hex(COLOR_TEXT), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_pos(title, 0, 0);

    // Rows below the title.
    int content_w = INFO_W - CARD_PAD * 2;
    int rows_top = TITLE_H + TITLE_GAP;
    for (int i = 0; i < DETECTION_UI_MAX_BARS; i++) {
        int row_y = rows_top + i * (ROW_H + ROW_GAP);
        create_bar_row(s_card, i, row_y, content_w);
    }
}

// ============================================================
// Public API
// ============================================================
void detection_ui_build(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COLOR_BG_PAGE), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    build_header(scr);
    build_camera_frame(scr);
    build_info_card(scr);
    build_footer(scr);

    // Pre-populate with varied placeholder values so the initial snapshot
    // shows what the bar chart looks like; real inference replaces this via
    // detection_ui_update().
    static const detection_t demo[] = {
        { .class_name = "Person",       .probability = 0.985f },
        { .class_name = "Car",          .probability = 0.762f },
        { .class_name = "Dog",          .probability = 0.541f },
        { .class_name = "Bus",          .probability = 0.318f },
        { .class_name = "Bird",         .probability = 0.142f },
        { .class_name = "sheep",        .probability = 0.582f },
        { .class_name = "motorcycle",   .probability = 0.242f },
    };
    detection_ui_update(demo, sizeof(demo) / sizeof(demo[0]));
}

void detection_ui_update(const detection_t *detections, int count)
{
    if (!s_card) return;

    int shown = count < DETECTION_UI_MAX_BARS ? count : DETECTION_UI_MAX_BARS;
    for (int i = 0; i < shown; i++) {
        bar_row_t *r = &s_bars[i];
        const detection_t *d = &detections[i];

        lv_obj_remove_flag(r->row, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(r->name_label, d->class_name ? d->class_name : "?");

        char pct_buf[16];
        float pct = d->probability * 100.0f;
        if (pct < 0.0f) pct = 0.0f;
        if (pct > 100.0f) pct = 100.0f;
        snprintf(pct_buf, sizeof(pct_buf), "%.1f%%", pct);
        lv_label_set_text(r->pct_label, pct_buf);

        int32_t permille = (int32_t)(d->probability * 1000.0f);
        if (permille < 0) permille = 0;
        if (permille > 1000) permille = 1000;
        lv_bar_set_value(r->bar, permille, LV_ANIM_OFF);
    }
    for (int i = shown; i < DETECTION_UI_MAX_BARS; i++) {
        lv_obj_add_flag(s_bars[i].row, LV_OBJ_FLAG_HIDDEN);
    }
}
