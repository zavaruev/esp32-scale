/*
 * ui.c - LVGL 9 interface.
 *
 * LVGL's own task (created by esp_lvgl_port) is pinned to core 1. It owns the
 * display and all widget access. This file additionally starts a slow task,
 * also on core 1, that copies the measurement snapshot into the widgets at
 * 10 Hz. Every widget write happens under lvgl_port_lock().
 *
 * The draw buffers are requested with buff_dma=true and buff_spiram=false:
 * SPI DMA on the S3 cannot read from PSRAM, so the buffers must live in
 * internal RAM even though the board has 8 MB of PSRAM.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include "audio.h"
#include "st7789.h"
#include "ui.h"
#include "weight_task.h"

static const char *TAG = "ui";

#define UI_TASK_STACK    4096
#define UI_TASK_PRIORITY 4
#define UI_TASK_CORE     1
#define UI_TASK_PERIOD_MS 100

/* Logical (post-rotation) UI size: landscape 320x240. The panel itself stays
 * 240x320; LVGL rotates the buffer in software/PPA. */
#define UI_HRES 320
#define UI_VRES 240

#define COLOR_BG      lv_color_hex(0x272729)
#define COLOR_WHITE   lv_color_hex(0xFFFFFF)
#define COLOR_MUTED   lv_color_hex(0x9A9AA0)
#define COLOR_BLUE    lv_color_hex(0x2997FF)
#define COLOR_GREEN   lv_color_hex(0x34C759)
#define COLOR_CHIP_BG lv_color_hex(0x333335)

static lv_display_t *s_disp;
static lv_obj_t *s_scr_weigh;
static lv_obj_t *s_scr_cal;
static lv_obj_t *s_lbl_weight;
static lv_obj_t *s_lbl_unit;
static lv_obj_t *s_lbl_wifi;
static lv_obj_t *s_lbl_cal_badge;
static lv_obj_t *s_chip_stable;
static lv_obj_t *s_chip_zero;
static lv_obj_t *s_chip_tare;
static lv_obj_t *s_chip_hold;
static lv_obj_t *s_lbl_cal_phase;
static lv_obj_t *s_cal_bar;
static lv_obj_t *s_lbl_cal_detail;

/* Hold state, mirrored from the web page so the bench and the browser agree. */
static bool s_locked;
static double s_locked_value;
static int s_quiet;
static double s_candidate;
static bool s_candidate_valid;
static bool s_was_locked;
static cal_phase_t s_last_cal_phase = CAL_IDLE;
static volatile bool s_ui_built = false;

static void style_chip(lv_obj_t *chip, bool on)
{
    lv_obj_set_style_text_color(chip, on ? COLOR_BLUE : COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_border_color(chip, on ? COLOR_BLUE : COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, on ? COLOR_CHIP_BG : COLOR_BG, LV_PART_MAIN);
}

static lv_obj_t *create_chip(lv_obj_t *parent, const char *text, int x)
{
    lv_obj_t *chip = lv_label_create(parent);
    lv_label_set_text(chip, text);
    lv_obj_set_pos(chip, x, 176);
    lv_obj_set_style_text_font(chip, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_pad_left(chip, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_right(chip, 7, LV_PART_MAIN);
    lv_obj_set_style_pad_top(chip, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(chip, 5, LV_PART_MAIN);
    lv_obj_set_style_radius(chip, 12, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
    style_chip(chip, false);
    return chip;
}

static void build_weigh_screen(void)
{
    s_scr_weigh = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_weigh, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr_weigh, LV_OBJ_FLAG_SCROLLABLE);

    /* Top status strip. */
    s_lbl_wifi = lv_label_create(s_scr_weigh);
    lv_label_set_text(s_lbl_wifi, "WIFI --");
    lv_obj_set_pos(s_lbl_wifi, 12, 10);
    lv_obj_set_style_text_color(s_lbl_wifi, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_wifi, &lv_font_montserrat_14, LV_PART_MAIN);

    s_lbl_cal_badge = lv_label_create(s_scr_weigh);
    lv_label_set_text(s_lbl_cal_badge, "");
    lv_obj_align(s_lbl_cal_badge, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_set_style_text_color(s_lbl_cal_badge, COLOR_BLUE, LV_PART_MAIN);
    lv_obj_set_style_text_font(s_lbl_cal_badge, &lv_font_montserrat_14, LV_PART_MAIN);

    /* The reading is the product: it dominates the screen. */
    s_lbl_weight = lv_label_create(s_scr_weigh);
    lv_label_set_text(s_lbl_weight, "0.00");
    lv_obj_align(s_lbl_weight, LV_ALIGN_TOP_MID, -14, 62);
    lv_obj_set_style_text_font(s_lbl_weight, &lv_font_montserrat_48, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_weight, COLOR_WHITE, LV_PART_MAIN);

    s_lbl_unit = lv_label_create(s_scr_weigh);
    lv_label_set_text(s_lbl_unit, "g");
    lv_obj_align(s_lbl_unit, LV_ALIGN_TOP_MID, 104, 88);
    lv_obj_set_style_text_font(s_lbl_unit, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_unit, COLOR_MUTED, LV_PART_MAIN);

    s_chip_stable = create_chip(s_scr_weigh, "STABLE", 12);
    s_chip_zero   = create_chip(s_scr_weigh, "ZERO", 88);
    s_chip_tare   = create_chip(s_scr_weigh, "TARE", 160);
    s_chip_hold   = create_chip(s_scr_weigh, "HOLD", 232);

    /* Soft-key legend along the bottom, in the same order as the panel. */
    lv_obj_t *keys = lv_label_create(s_scr_weigh);
    lv_label_set_text(keys,
        "ED      SUM     ITT     SBR\n"
        "0       T       AZT     CAL");
    lv_obj_align(keys, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_text_font(keys, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(keys, COLOR_MUTED, LV_PART_MAIN);
    lv_obj_set_style_text_align(keys, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
}

static void build_cal_screen(void)
{
    s_scr_cal = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr_cal, COLOR_BG, LV_PART_MAIN);
    lv_obj_clear_flag(s_scr_cal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_scr_cal);
    lv_label_set_text(title, "CALIBRATION");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(title, COLOR_WHITE, LV_PART_MAIN);

    s_lbl_cal_phase = lv_label_create(s_scr_cal);
    lv_label_set_text(s_lbl_cal_phase, "Starting...");
    lv_obj_align(s_lbl_cal_phase, LV_ALIGN_TOP_MID, 0, 84);
    lv_obj_set_style_text_font(s_lbl_cal_phase, &lv_font_montserrat_16, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_cal_phase, COLOR_BLUE, LV_PART_MAIN);

    s_cal_bar = lv_bar_create(s_scr_cal);
    lv_obj_set_size(s_cal_bar, 260, 10);
    lv_obj_align(s_cal_bar, LV_ALIGN_TOP_MID, 0, 124);
    lv_obj_set_style_bg_color(s_cal_bar, lv_color_hex(0x3A3A3C), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_cal_bar, COLOR_BLUE, LV_PART_INDICATOR);
    lv_bar_set_range(s_cal_bar, 0, 100);

    s_lbl_cal_detail = lv_label_create(s_scr_cal);
    lv_label_set_text(s_lbl_cal_detail, "");
    lv_obj_align(s_lbl_cal_detail, LV_ALIGN_TOP_MID, 0, 154);
    lv_obj_set_style_text_font(s_lbl_cal_detail, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_lbl_cal_detail, COLOR_MUTED, LV_PART_MAIN);

    lv_obj_t *hint = lv_label_create(s_scr_cal);
    lv_label_set_text(hint, "SBR - cancel");
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_color(hint, COLOR_MUTED, LV_PART_MAIN);
}

static double hold_or_show(double grams, bool stable)
{
    if (!s_candidate_valid) {
        s_candidate = grams;
        s_candidate_valid = true;
    }
    if (s_locked) {
        if (fabs(grams - s_locked_value) > 0.5) {
            s_locked = false;
            s_quiet = 0;
            s_candidate = grams;
        } else {
            return s_locked_value;
        }
    }
    if (fabs(grams - s_candidate) <= 0.03) {
        s_quiet++;
    } else {
        s_quiet = 0;
        s_candidate = grams;
    }
    if (s_quiet >= 3 && stable) {
        s_locked = true;
        s_locked_value = grams;
    }
    return s_locked ? s_locked_value : grams;
}

static void update_weigh(const scale_snapshot_t *s)
{
    double per = 1.0;
    int decimals = 2;
    const char *unit = "g";
    if (s->unit == UNIT_KILOGRAM) {
        per = 1000.0; decimals = 3; unit = "kg";
    } else if (s->unit == UNIT_OUNCE) {
        per = 28.349523125; decimals = 3; unit = "oz";
    }

    double shown;
    if (s->zero) {
        s_locked = false;
        s_quiet = 0;
        s_candidate_valid = false;
        shown = 0.0;
    } else {
        shown = hold_or_show(s->avg_grams, s->stable);
    }
    shown /= per;

    char buf[32];
    snprintf(buf, sizeof(buf), "%.*f", decimals, shown);
    if (strcmp(buf, lv_label_get_text(s_lbl_weight)) != 0) {
        lv_label_set_text(s_lbl_weight, buf);
    }
    if (strcmp(unit, lv_label_get_text(s_lbl_unit)) != 0) {
        lv_label_set_text(s_lbl_unit, unit);
    }

    bool settled = (s_locked || (s->zero && s->stable)) && s->avg_n >= 45;
    lv_obj_set_style_text_color(s_lbl_weight,
                                settled ? COLOR_GREEN : COLOR_WHITE, LV_PART_MAIN);

    style_chip(s_chip_stable, s->stable);
    style_chip(s_chip_zero, s->zero);
    style_chip(s_chip_tare, s->tare_active);
    style_chip(s_chip_hold, s_locked);
}

static void update_cal(void)
{
    cal_state_t c;
    weight_task_cal_get(&c);

    if (c.phase != s_last_cal_phase) {
        s_last_cal_phase = c.phase;
        if (c.phase == CAL_WAIT_LOAD) {
            audio_play(AUDIO_PLACE_WEIGHT);
        } else if (c.phase == CAL_DONE) {
            audio_play(AUDIO_CAL_DONE);
        } else if (c.phase == CAL_ABORT) {
            audio_play(AUDIO_ERROR);
        }
    }

    const char *text;
    int progress;
    switch (c.phase) {
    case CAL_SETTLING: text = "Empty the platform"; progress = 15; break;
    case CAL_WAIT_LOAD: text = "Place the reference"; progress = 50; break;
    case CAL_MEASURING: text = "Measuring..."; progress = 85; break;
    case CAL_DONE:      text = "Done"; progress = 100; break;
    case CAL_ABORT:     text = "Cancelled"; progress = 100; break;
    default:            text = "Starting..."; progress = 5; break;
    }
    if (strcmp(text, lv_label_get_text(s_lbl_cal_phase)) != 0) {
        lv_label_set_text(s_lbl_cal_phase, text);
    }
    lv_bar_set_value(s_cal_bar, progress, LV_ANIM_OFF);

    char detail[96];
    if (c.phase == CAL_DONE && c.new_cal > 0) {
        snprintf(detail, sizeof(detail), "new cal = %.2f", c.new_cal);
    } else if (c.grams > 0) {
        snprintf(detail, sizeof(detail), "%.2f g", c.grams);
    } else {
        detail[0] = '\0';
    }
    if (strcmp(detail, lv_label_get_text(s_lbl_cal_detail)) != 0) {
        lv_label_set_text(s_lbl_cal_detail, detail);
    }
}

static void ui_refresh_cb(lv_timer_t *timer);

/* Builds both screens once on core 1, then deletes itself. */
static void ui_build_task(void *arg)
{
    (void)arg;
    lvgl_port_lock(0);
    build_weigh_screen();
    build_cal_screen();
    lv_screen_load(s_scr_weigh);
    /* The refresh timer is created here, while we hold the LVGL mutex and run
     * on core 1, so all widget writes happen on the LVGL task itself. */
    lv_timer_create(ui_refresh_cb, UI_TASK_PERIOD_MS, NULL);
    lvgl_port_unlock();
    s_ui_built = true;   /* publish only after the screens are fully built */
    vTaskDelete(NULL);
}

/* Periodic refresh runs INSIDE the LVGL task via lv_timer, not in a separate
 * FreeRTOS task. Two tasks on core 1 both taking lvgl_port_lock(0) (an
 * indefinite recursive mutex) can deadlock after minutes: the LVGL task stops
 * reaching lv_timer_handler(), flush stops, and the panel keeps its last frame
 * until the backlight is the only thing still driven - i.e. a black screen that
 * appears after a while and survives a power cycle only by luck. One task owns
 * the display, so there is nothing to contend with. */
static void ui_refresh_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ui_built) {
        return;
    }
    scale_snapshot_t s;
    weight_task_get(&s);

    if (lv_screen_active() == s_scr_weigh) {
        update_weigh(&s);
        if (s_locked && !s_was_locked && !s.zero) {
            audio_play(AUDIO_STABLE);
        }
        s_was_locked = s_locked;
    } else if (lv_screen_active() == s_scr_cal) {
        update_cal();
    }
}

esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 4;
    port_cfg.task_stack = 7168;
    port_cfg.task_affinity = UI_TASK_CORE;   /* keep every LVGL callback off core 0 */
    port_cfg.task_max_sleep_ms = 50;
    port_cfg.timer_period_ms = 5;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init: %s", esp_err_to_name(err));
        return err;
    }

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = UI_HRES * 40,
        .double_buffer = true,
        .hres = UI_HRES,
        .vres = UI_VRES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            /* Apply the mount rotation ONCE here, at ROTATION_0, which is what
             * lvgl_port_add_disp() does. A later lv_display_set_rotation()
             * would apply swap_xy again (inverted) and desync the buffer size
             * from the panel -> black screen.
             * swap_xy + mirror_x == the old working ROTATION_270 layout. */
            .swap_xy = true,
            .mirror_x = true,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,   /* SPI DMA cannot read PSRAM on the S3 */
            .swap_bytes = false,
            .sw_rotate = false,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (!s_disp) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    /* Build the screens and load the first one on CORE 1, never in app_main.
     *
     * This used to happen right here in ui_init(), which runs on Core 0. A
     * single lv_screen_load() walks every widget and invalidates areas, which
     * took long enough to starve IDLE0 and trip the 5 s task watchdog:
     *   app_main -> ui_init -> lv_screen_load -> lv_obj_set_pos -> lv_inv_area
     * Running the same code in a task pinned to Core 1, under the port's mutex,
     * is both correct (LVGL's task owns the display) and keeps Core 0 free for
     * the HX711 and the console. */
    BaseType_t ok = xTaskCreatePinnedToCore(ui_build_task, "ui_build", UI_TASK_STACK,
                                             NULL, UI_TASK_PRIORITY, NULL, UI_TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "ui_build task creation failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "LVGL ready, %dx%d (panel %dx%d)", UI_HRES, UI_VRES,
             ST7789_NATIVE_W, ST7789_NATIVE_H);
    return ESP_OK;
}

void ui_show_page(ui_page_t page)
{
    if (!s_ui_built) {
        return;
    }
    lvgl_port_lock(0);
    lv_screen_load(page == UI_PAGE_CAL ? s_scr_cal : s_scr_weigh);
    lvgl_port_unlock();
}
