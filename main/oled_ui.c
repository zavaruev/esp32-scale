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

    /*.Width of a 7-seg string =sum(13 per digit, 5 for '.', 11 for '-'). */
    /* Layout in the 64 px wide portrait window:
     *   < 1 kg : big grams value / "g"
     *   >=1 kg : big kg value / "kg" / big g remainder / "g"             */
    char buf[24];

    if (grams_now < 1000.0) {
        int v = (int)(grams_now + 0.5);
        snprintf(buf, sizeof(buf), "%d", v);
        int w = 0;
        for (char *p = buf; *p; p++) w += (*p == '.') ? 5 : 13;
        oled_7seg((OLED_WIDTH - w) / 2, 40, buf, true);
        oled_text((OLED_WIDTH - 6) / 2, 76, "g", true);
    } else {
        int kg = (int)(grams_now / 1000.0);
        int rem = (int)(grams_now - kg * 1000.0 + 0.5);
        if (rem >= 1000) { kg++; rem -= 1000; }
        snprintf(buf, sizeof(buf), "%d", kg);
        int w1 = (int)strlen(buf) * 13;
        oled_7seg((OLED_WIDTH - w1) / 2, 14, buf, true);
        oled_text((OLED_WIDTH - 12) / 2, 40, "kg", true);

        snprintf(buf, sizeof(buf), "%d", rem);
        int w2 = (int)strlen(buf) * 13;
        oled_7seg((OLED_WIDTH - w2) / 2, 56, buf, true);
        oled_text((OLED_WIDTH - 6) / 2, 82, "g", true);
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
