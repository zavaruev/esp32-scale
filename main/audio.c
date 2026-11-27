/*
 * audio.c - active buzzer on GPIO47, low-level triggered.
 *
 * Wiring: VCC -> 3V3, GND -> GND, I/O -> GPIO47.
 * The buzzer has its own oscillator; I/O LOW turns it on, HIGH turns it off.
 * Cues are therefore patterns of (on_ms, off_ms) pairs. No PWM and no audio
 * files. Everything runs on core 1; core 0 stays dedicated to the HX711.
 */
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio.h"

static const char *TAG = "audio";

#define BUZZER_PIN      GPIO_NUM_47

/* The module is marked "低电平触发" = triggered by a LOW level. */
#define BUZZ_ON()       gpio_set_level(BUZZER_PIN, 0)
#define BUZZ_OFF()      gpio_set_level(BUZZER_PIN, 1)

#define TASK_STACK      2048
#define TASK_PRIORITY   3
#define TASK_CORE       1

/* A step: how long the buzzer is ON, then how long it is OFF. */
typedef struct {
    uint16_t on_ms;
    uint16_t off_ms;
} step_t;

typedef struct {
    step_t steps[4];   /* a 0/0 step terminates the pattern */
} cue_t;

static const cue_t s_cues[AUDIO_COUNT] = {
    /* TARE */         {{{70, 0},   {0, 0}, {0, 0}, {0, 0}}},
    /* STABLE */       {{{140, 0},  {0, 0}, {0, 0}, {0, 0}}},
    /* AZT_ON  */      {{{60, 80},  {60, 0}, {0, 0}, {0, 0}}},
    /* AZT_OFF */      {{{60, 160}, {60, 0}, {0, 0}, {0, 0}}},
    /* CAL_START */    {{{60, 60},  {60, 60}, {60, 0}, {0, 0}}},
    /* PLACE_WEIGHT */ {{{400, 0},  {0, 0}, {0, 0}, {0, 0}}},
    /* CAL_DONE */     {{{80, 60},  {80, 60}, {160, 0}, {0, 0}}},
    /* ERROR */        {{{220, 120}, {220, 120}, {220, 0}, {0, 0}}},
};

static const char *const s_names[AUDIO_COUNT] = {
    "tare", "stable", "azt_on", "azt_off",
    "cal_start", "place_weight", "cal_done", "error",
};

static volatile audio_cue_t s_pending = AUDIO_COUNT;   /* nothing queued */

static void audio_task(void *arg)
{
    (void)arg;
    while (1) {
        if (s_pending == AUDIO_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        audio_cue_t cue = s_pending;
        s_pending = AUDIO_COUNT;   /* consume before playing so new cues win */

        const cue_t *c = &s_cues[cue];
        for (int i = 0; i < 4; i++) {
            if (c->steps[i].on_ms == 0 && c->steps[i].off_ms == 0) {
                break;
            }
            BUZZ_ON();
            vTaskDelay(pdMS_TO_TICKS(c->steps[i].on_ms));
            BUZZ_OFF();
            if (c->steps[i].off_ms) {
                vTaskDelay(pdMS_TO_TICKS(c->steps[i].off_ms));
            }
        }
        BUZZ_OFF();   /* ensure it is silent at the end */
        ESP_LOGI(TAG, "played %s", s_names[cue]);
    }
}

esp_err_t audio_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << BUZZER_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config: %s", esp_err_to_name(err));
        return err;
    }
    BUZZ_OFF();   /* idle high = buzzer off (low-level triggered) */

    BaseType_t ok = xTaskCreatePinnedToCore(audio_task, "audio", TASK_STACK,
                                             NULL, TASK_PRIORITY, NULL, TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "active buzzer ready on GPIO%d (VCC=3V3, GND=GND)", BUZZER_PIN);
    /* Power-on chirp: makes the buzzer and its wiring checkable without a PC
     * or a connected keypad - just apply power and listen. */
    audio_play(AUDIO_TARE);
    return ESP_OK;
}

void audio_play(audio_cue_t cue)
{
    if ((int)cue >= 0 && cue < AUDIO_COUNT) {
        s_pending = cue;
    }
}

void audio_test(int index)
{
    if (index >= 0 && index < AUDIO_COUNT) {
        audio_play((audio_cue_t)index);
    }
}
