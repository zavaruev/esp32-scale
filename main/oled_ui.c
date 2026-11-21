/*
 * oled_ui.c - weighing screen on the 64x128 portrait OLED.
 *
 * Portrait layout (top to bottom):
 *   - unit (g / kg / oz), small label
 *   - the weight: integer part in BIG digits, fraction in small digits
 *   - status flags (STB / ZER / TRE / HLD)
 * One task reads the snapshot and redraws at ~7 Hz. No LVGL, no mutex.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"
#include "oled_ui.h"
#include "ssd1306.h"
#include "weight_task.h"

static const char *TAG = "oled_ui";

#define UI_PERIOD_MS 150

/* Load cell is rated 6 kg. Above this the reading is not trustworthy, so show
 * a clear OVER indicator instead of a plausible-looking number. */
#define OVERLOAD_GRAMS 6000.0f

static bool s_locked;
static double s_locked_value, s_candidate;
static int s_quiet;
static bool s_candidate_valid;
static bool s_was_locked;   /* to beep once on the rising edge of HOLD */
static bool s_was_overload; /* to beep once when overload appears */
static bool s_overload;     /* current state, set during draw_screen */

/* A small check mark drawn with two strokes, width ~9 px. */
static void draw_check(int x, int y, bool on)
{
    /* short stroke down-right */
    for (int i = 0; i < 4; i++) oled_fill_rect(x + i, y + 4 + i, 2, 2, on);
    /* long stroke up-right */
    for (int i = 0; i < 7; i++) oled_fill_rect(x + 3 + i, y + 7 - i, 2, 2, on);
}

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
    s_overload = false;

    const char *unit = (s->unit == UNIT_KILOGRAM) ? "kg" :
                       (s->unit == UNIT_OUNCE) ? "oz" : "g";
    double per = (s->unit == UNIT_KILOGRAM) ? 1000.0 :
                 (s->unit == UNIT_OUNCE) ? 28.349523125 : 1.0;
    int decimals = (s->unit == UNIT_GRAM) ? 2 : 3;

    double shown = s->zero ? 0.0 : hold_or_show(s->avg_grams, s->stable);
    shown /= per;

    /* Overload: the value is beyond the 6 kg cell range. Show a clear warning
     * instead of a number that looks valid. Compare in grams regardless of the
     * selected display unit. */
    double shown_grams = shown * per;
    if (!s->zero && shown_grams >= OVERLOAD_GRAMS) {
        s_overload = true;
        oled_text((OLED_WIDTH - 24) / 2, 26, "OVER", true);
        oled_text((OLED_WIDTH - 24) / 2, 50, "LOAD", true);
        /* A warning triangle-ish mark: filled bar + exclamation. */
        oled_fill_rect(22, 76, 20, 2, true);
        oled_text((OLED_WIDTH - 6) / 2, 82, "!", true);
        return;   /* skip the normal weight layout */
    }

    /* Two layouts, unit always on its OWN line under the number:
     *   < 1 kg : big number / "g"
     *   >=1 kg : big "N" / "kg" / big remainder / "g"
     * All strings are width-checked against OLED_WIDTH (64 px). */
    char line1[24], line2[24];
    bool over_kg = (s->unit == UNIT_GRAM) ? (shown >= 1000.0) :
                   (s->unit == UNIT_KILOGRAM) ? (shown >= 1.0) :
                   (shown * 28.349523125 >= 1000.0);

    if (over_kg) {
        /* Work in kg + grams remainder regardless of selected unit. */
        double grams_total = shown * per;
        int kg = (int)(grams_total / 1000.0);
        int rem = (int)(grams_total - kg * 1000.0 + 0.5);
        if (rem >= 1000) { kg++; rem -= 1000; }
        snprintf(line1, sizeof(line1), "%d", kg);
        snprintf(line2, sizeof(line2), "%d", rem);

        /* Line 1: big kilograms. */
        int wkg = (int)strlen(line1) * 18;
        int xkg = (OLED_WIDTH - wkg) / 2;
        if (xkg < 0) xkg = 0;
        oled_big_text(xkg, 14, line1, true);
        /* Line 2: "kg". */
        oled_text((OLED_WIDTH - 12) / 2, 38, "kg", true);
        /* Line 3: big grams remainder. */
        int wg = (int)strlen(line2) * 18;
        int xg = (OLED_WIDTH - wg) / 2;
        if (xg < 0) xg = 0;
        oled_big_text(xg, 52, line2, true);
        /* Line 4: "g". */
        oled_text((OLED_WIDTH - 6) / 2, 76, "g", true);
    } else {
        /* Under 1 kg: show grams (or selected unit) as an integer.
         * Up to 3 digits fit in BIG (3*18=54 px). 4+ digits fall back to the
         * small font so nothing is clipped at 64 px wide. */
        double v = shown * per;
        snprintf(line1, sizeof(line1), "%d", (int)(v + 0.5));
        int digits = (int)strlen(line1);
        if (digits <= 3) {
            int w1 = digits * 18;
            int x1 = (OLED_WIDTH - w1) / 2;
            if (x1 < 0) x1 = 0;
            if (shown < 0) oled_text(1, 30, "-", true);
            oled_big_text(x1, 22, line1, true);
        } else {
            int w1 = digits * 6;
            int x1 = (OLED_WIDTH - w1) / 2;
            if (x1 < 0) x1 = 0;
            oled_text(x1, 30, line1, true);
        }
        /* Unit under the number, smaller. */
        int uw = (int)strlen(unit) * 6;
        oled_text((OLED_WIDTH - uw)/2, 50, unit, true);
    }

    /* When the reading has settled, show a check mark next to the unit. */
    if (s_locked) {
        if (over_kg) {
            /* next to the "g" line (y=76), or next to "kg" if you prefer */
            draw_check((OLED_WIDTH - 6) / 2 + 8, 76, true);
        } else {
            int uw = (int)strlen(unit) * 6;
            int ux = (OLED_WIDTH - uw) / 2;
            draw_check(ux + uw + 3, 50, true);
        }
    }

    /* Status flags at the bottom. */
    int bx = 2;
    if (s->stable)      bx = oled_text(bx, 108, "STB ", true);
    if (s->zero)        bx = oled_text(bx, 108, "ZER ", true);
    if (s->tare_active) bx = oled_text(bx, 108, "TRE ", true);
    if (s_locked)       bx = oled_text(bx, 108, "HLD", true);
    oled_fill_rect(4, 104, OLED_WIDTH - 8, 1, true);
}

static void oled_ui_task(void *arg)
{
    (void)arg;
    while (1) {
        scale_snapshot_t s;
        weight_task_get(&s);
        draw_screen(&s);
        /* Warning beep the moment the reading goes over the 6 kg limit. */
        if (s_overload && !s_was_overload) {
            audio_play(AUDIO_ERROR);
        }
        s_was_overload = s_overload;

        /* One short beep when the weight first settles. Skip the zero case and
         * overload: an empty platform locks instantly, and an overloaded one is
         * already covered by the warning. */
        if (s_locked && !s_was_locked && !s.zero && !s_overload) {
            audio_play(AUDIO_STABLE);
        }
        s_was_locked = s_locked;
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
    ESP_LOGI(TAG, "OLED UI ready (portrait, big weight)");
    return ESP_OK;
}
