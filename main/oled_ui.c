/*
 * oled_ui.c - weighing screen on the 64x128 portrait OLED.
 *
 * Portrait layout (top to bottom), weights >= 1 kg:
 *   - big kilogram value, then small "kg" label
 *   - big gram remainder value, then small "g" label
 *   - (below 1 kg: big value and small "g" label)
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

    double grams_now = shown * per;      /* grams, regardless of selected unit */

    if (!s->zero && grams_now >= OVERLOAD_GRAMS) {
        s_overload = true;
        oled_text((OLED_WIDTH - 24) / 2, 50, "OVER", true);
        return;
    }

    /* Helper: draw a string with the BIG font (5x7 x 2). Fit to width by
     * choosing scale 3 (3 digits, 54 px) or 2 (4 digits, 48 px). */
    char buf[24];
    /* Bold small label: slightly bigger than the plain 5x7, pitch 7. */
    int draw_small(int y, const char *txt)
    {
        int w = (int)strlen(txt) * 7;
        oled_text_bold((OLED_WIDTH - w) / 2, y, txt);
        return 0;
    }
    /* Big 5x7x3 line centered on the screen. */
    int draw_big(int y, const char *txt)
    {
        int n = (int)strlen(txt);
        int w = (n <= 3) ? n * 18 : n * 12;
        oled_big_text((OLED_WIDTH - w) / 2, y, txt, true);
        return 0;
    }

    /* Screen is 128 rows: labels sit in the bottom lines, like the old
     * design where they were drawn at the bottom of the panel. */
    if (grams_now < 1000.0) {
        /* < 1 kg: big value, small "g" label right under it. */
        snprintf(buf, sizeof(buf), "%d", (int)(grams_now + 0.5));
        draw_big(2, buf);
        draw_small(26, "g");
    } else {
        /* >= 1 kg: big kg value, small "kg" label under it, big grams
         * value, small "g" label under it - labels follow their digits. */
        int kg = (int)(grams_now / 1000.0);
        int rem = (int)(grams_now - kg * 1000.0 + 0.5);
        if (rem >= 1000) { kg++; rem -= 1000; }
        snprintf(buf, sizeof(buf), "%d", kg);
        draw_big(2, buf);
        draw_small(26, "kg");
        snprintf(buf, sizeof(buf), "%d", rem);
        draw_big(38, buf);
        draw_small(62, "g");
    }

    /* Status chips along the bottom, small bold 5x7, names matching the web
     * page badges: STABLE, ZERO, TARE, AVG, HOLD. */
    {
        int cx = 0, cy = 90;
        if (s->stable)      cx = oled_text_bold(cx, cy, "STABLE") + 2;
        if (s->zero)        cx = oled_text_bold(cx, cy, "ZERO") + 2;
        if (s->tare_active) cx = oled_text_bold(cx, cy, "TARE") + 2;
        if (s->avg_n >= 45) cx = oled_text_bold(cx, cy, "AVG") + 2;
        if (s_locked)       cx = oled_text_bold(cx, cy, "HOLD");
    }
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
