/*
 * buttons.c - matrix scan with debounce and short/long press detection.
 *
 * Timing: the task wakes every 5 ms, a key must stay stable for 4 scans (20 ms)
 * before it is accepted, and 1500 ms of continuous press produces BTN_LONG
 * instead of BTN_SHORT. Long is emitted exactly once, on the threshold.
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "buttons.h"

static const char *TAG = "buttons";

#define ROW1 GPIO_NUM_6   /* ribbon contact 1 */
#define ROW2 GPIO_NUM_13   /* ribbon contact 5 */
#define COL2 GPIO_NUM_8   /* ribbon contact 2 */
#define COL3 GPIO_NUM_9   /* ribbon contact 3 */
#define COL4 GPIO_NUM_7  /* ribbon contact 4 */

#define SCAN_PERIOD_MS   5
#define DEBOUNCE_SCANS   4
#define LONG_PRESS_MS    1500
#define TASK_STACK       3072
#define TASK_PRIORITY    3
#define TASK_CORE        1

typedef struct {
    gpio_num_t row;
    gpio_num_t col;
    button_id_t id;
} key_def_t;

static const key_def_t s_keys[BTN_COUNT] = {
    {ROW1, COL2, BTN_ED},
    {ROW1, COL3, BTN_SUM},
    {ROW1, COL4, BTN_ZERO},
    {ROW2, COL2, BTN_T},
    {ROW2, COL3, BTN_ITT},
    {ROW2, COL4, BTN_SBR},
};

static const char *const s_names[BTN_COUNT] = {
    "ED", "SUM", "ZERO", "T", "ITT", "SBR",
};

static button_handler_t s_handler;

/* Per-key state, touched only by the scanning task. */
static struct {
    bool settled;        /* debounced state */
    bool raw;            /* last raw reading */
    int  raw_count;      /* consecutive raw readings equal to `raw` */
    int64_t pressed_us;  /* when the current press started */
    bool long_sent;
} s_state[BTN_COUNT];

static void configure_gpio(void)
{
    gpio_config_t col_cfg = {
        .pin_bit_mask = (1ULL << COL2) | (1ULL << COL3) |
                        (1ULL << COL4),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&col_cfg);

    /* Rows start as inputs, i.e. floating. scan_key() briefly turns one into an
     * output and puts it back. */
    gpio_config_t row_cfg = {
        .pin_bit_mask = (1ULL << ROW1) | (1ULL << ROW2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&row_cfg);
}

/* Returns true when the key at (row,col) is closed. */
static bool scan_key(gpio_num_t row, gpio_num_t col)
{
    gpio_set_direction(row, GPIO_MODE_OUTPUT);
    gpio_set_level(row, 0);
    esp_rom_delay_us(100);
    bool pressed = (gpio_get_level(col) == 0);
    /* Floating again BEFORE the next row is driven. */
    gpio_set_direction(row, GPIO_MODE_INPUT);
    return pressed;
}

static void buttons_task(void *arg)
{
    (void)arg;
    while (1) {
        for (int i = 0; i < BTN_COUNT; i++) {
            bool raw = scan_key(s_keys[i].row, s_keys[i].col);

            if (raw != s_state[i].raw) {
                s_state[i].raw = raw;
                s_state[i].raw_count = 0;
            } else if (s_state[i].raw_count < DEBOUNCE_SCANS) {
                s_state[i].raw_count++;
            }

            if (s_state[i].raw_count >= DEBOUNCE_SCANS &&
                raw != s_state[i].settled) {
                s_state[i].settled = raw;
                if (raw) {
                    s_state[i].pressed_us = esp_timer_get_time();
                    s_state[i].long_sent = false;
                } else if (!s_state[i].long_sent) {
                    int64_t held = esp_timer_get_time() - s_state[i].pressed_us;
                    if (held >= 30000 && s_handler) {
                        ESP_LOGI(TAG, "%s SHORT (%lld ms)",
                                 s_names[i], (long long)(held / 1000));
                        s_handler(s_keys[i].id, BTN_SHORT);
                    }
                }
            }

            if (s_state[i].settled && !s_state[i].long_sent) {
                int64_t held = esp_timer_get_time() - s_state[i].pressed_us;
                if (held >= (int64_t)LONG_PRESS_MS * 1000LL) {
                    s_state[i].long_sent = true;
                    if (s_handler) {
                        ESP_LOGI(TAG, "%s LONG", s_names[i]);
                        s_handler(s_keys[i].id, BTN_LONG);
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(SCAN_PERIOD_MS));
    }
}

esp_err_t buttons_init(button_handler_t handler)
{
    s_handler = handler;
    configure_gpio();
    memset(s_state, 0, sizeof(s_state));

    BaseType_t ok = xTaskCreatePinnedToCore(buttons_task, "buttons", TASK_STACK,
                                             NULL, TASK_PRIORITY, NULL, TASK_CORE);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to create task");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "matrix ready (2 rows x 3 cols, 6 keys)");
    return ESP_OK;
}

void buttons_dump(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        printf("#BTN,%s,row=%d,col=%d,state=%d\n",
               s_names[i], s_keys[i].row, s_keys[i].col,
               (int)scan_key(s_keys[i].row, s_keys[i].col));
    }
}
