/*
 * oled_ui.c - weighs screen on the 128x64 OLED.
 *
 * A single task reads the published snapshot and redraws the whole framebuffer
 * at ~7 Hz. No LVGL, no draw mutex, no second owner of the display.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "oled_ui.h"
#include "ssd1306.h"
#include "weight_task.h"

static const char *TAG = "oled_ui";

#define UI_PERIOD_MS 150

/* Hold logic, mirroring the web page, so all displays agree. */
static bool s_locked;
static double s_locked_value, s_candidate;
static int s_quiet;
static bool s_candidate_valid;

static double hold_or_show(double grams, bool stable)
{
    if (!s_candidate_valid) { s_candidate = grams; s_candidate_valid = true; }
    if (s_locked) {
        if (fabs(grams - s_locked_value) > 0.5) {
            s_locked = false; s_quiet = 0; s_candidate = grams;
        } else {
            return s_locked_value;
        }
    }
    if (fabs(grams - s_candidate) <= 0.03) s_quiet++;
    else { s_quiet = 0; s_candidate = grams; }
    if (s_quiet >= 3 && stable) { s_locked = true; s_locked_value = grams; }
    return s_locked ? s_locked_value : grams;
}

static void draw_screen(const scale_snapshot_t *s)
{
    oled_clear();

    /* Status line: unit + flags */
    const char *unit = (s->unit == UNIT_KILOGRAM) ? "kg" :
                       (s->unit == UNIT_OUNCE) ? "oz" : "g";
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "SCALE  %s", unit);
    oled_text(0, 0, hdr, true);

    /* Big weight, centered in the 128 px width.
     * Format to 2 decimals in g (3 in kg/oz); at 12 px per glyph a string like
     * "123.45" is ~72 px, which fits with the unit below. */
    double per = (s->unit == UNIT_KILOGRAM) ? 1000.0 :
                 (s->unit == UNIT_OUNCE) ? 28.349523125 : 1.0;
    int decimals = (s->unit == UNIT_GRAM) ? 2 : 3;

    double shown = s->zero ? 0.0 : hold_or_show(s->avg_grams, s->stable);
    shown /= per;

    char num[24];
    snprintf(num, sizeof(num), "%.*f", decimals, shown);
    /* big glyphs are 12 px wide; center the string. */
    int w = (int)strlen(num) * 12;
    int x = (OLED_WIDTH - w) / 2;
    if (x < 0) x = 0;
    oled_big_text(x, 20, num, true);

    /* Status chips along the bottom (small 5x7 font). */
    int bx = 0;
    if (s->stable) { bx = oled_text(bx, 56, "STB ", true); }
    if (s->zero)   { bx = oled_text(bx, 56, "ZER ", true); }
    if (s->tare_active) { bx = oled_text(bx, 56, "TRE ", true); }
    if (s_locked)  { bx = oled_text(bx, 56, "HLD", true); }
}

static void oled_ui_task(void *arg)
{
    (void)arg;
    while (1) {
        scale_snapshot_t s;
        weight_task_get(&s);
        draw_screen(&s);
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(UI_PERIOD_MS));
    }
}

esp_err_t oled_ui_init(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(oled_ui_task, "oled_ui", 3072,
                                             NULL, 4, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "OLED UI ready");
    return ESP_OK;
}
