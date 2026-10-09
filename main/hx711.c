/*
 * hx711.c - bit-banged driver for the HX711 24-bit sigma-delta ADC
 *
 * SCK idles LOW. DOUT is pulled LOW by the chip when a conversion is ready.
 * The 24 result bits are clocked out MSB-first; each bit is sampled while SCK
 * is high, after the rising-edge settling delay.
 */
#include "hx711.h"

#include <stdint.h>

#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Settling delay after each SCK edge before sampling DOUT.
 *
 * Reduced from 20 us to 5 us. The datasheet allows an SCK period up to 1 MHz,
 * i.e. 1 us, so 5 us carries a 5x margin over the chip's own limit. The reason
 * to shorten it is the critical section below: the read is protected by masking
 * interrupts, and the masked window is 24 bits x 2 edges x this value. At 20 us
 * it was ~1 ms of interrupts off, which is long enough to disturb WiFi; at 5 us
 * it is ~250 us, which is comfortably brief. */
#define HX711_SETTLE_US 5

/* How long the chip is allowed to spend a conversion. Generous upper bound:
 * the slowest supported rate is 10 SPS, i.e. one sample per 100 ms. */
#define HX711_READY_TIMEOUT_US 250000

/* There is NO configuration register on this revision. Both wrong assumptions
 * that this file previously encoded are removed:
 *
 *  1. "Write CR1 to set the rate." CR1 does not exist. Rate is set by the LEVEL
 *     on physical pin 15 (RATE): 0 = 10 Hz, 1 = 80 Hz. No software can change
 *     it, which is why a brute-force sweep of all 128 candidate register bytes
 *     produced no change in rate. Getting 80 SPS requires wiring that pin.
 *
 *  2. "Write CR1 to set gain." Gain and channel are selected by the COUNT of
 *     PD_SCK pulses in one conversion period: 25 = ch A gain 128, 26 = ch B
 *     gain 32, 27 = ch A gain 64. The datasheet warns that anything outside
 *     25-27 causes a serial communication error, so the read below emits
 *     exactly 25 and no configuration writes may add more.
 *
 * Reference: hx711_english.pdf (Avia Semiconductor), Table 1 pin 15, Table 3.
 */
#define HX711_PULSES_CH_A_GAIN_128 25
#define HX711_DATA_BITS 24

/* On by default: see the comment on portENTER_CRITICAL in hx711_read_raw. */
static bool irq_guard = true;

/* Measured length of the interrupt-masked window, and the worst one seen. */
int64_t hx711_mask_last_us;
int64_t hx711_mask_max_us;

void hx711_set_irq_guard(bool on)
{
    irq_guard = on;
}

static inline void clock_high(const hx711_t *dev)
{
    gpio_set_level(dev->clock_pin, 1);
}

static inline void clock_low(const hx711_t *dev)
{
    gpio_set_level(dev->clock_pin, 0);
}

static inline bool data_high(const hx711_t *dev)
{
    return gpio_get_level(dev->data_pin) != 0;
}

void hx711_init(hx711_t *dev, gpio_num_t data_pin, gpio_num_t clock_pin)
{
    dev->data_pin = data_pin;
    dev->clock_pin = clock_pin;
    portMUX_INITIALIZE(&dev->mux);

    /* Clock first, held low, then release DOUT as a pulled-down input. If DOUT
     * went high-Z while SCK was high, the floating line can produce a spurious
     * edge that the ADC would clock in as a real bit. */
    gpio_config_t clk = {
        .pin_bit_mask = 1ULL << clock_pin,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&clk));
    clock_low(dev);

    gpio_config_t dta = {
        .pin_bit_mask = 1ULL << data_pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&dta));
}

bool hx711_read_raw(hx711_t *dev, int32_t *raw, int64_t *wait_us)
{
    int64_t wait_start = esp_timer_get_time();
    /* DOUT is pulled LOW once a conversion is complete.
     *
     * This loop is the task's normal state, not an exception: the chip converts
     * in ~88 ms at 11.3 Hz, so the wait covers nearly the whole sample period.
     *
     * It must yield, not spin. Two measured consequences of busy-waiting here:
     * the task held CPU 0 at priority 5 for ~88 ms out of every 100 ms, which
     * starved IDLE0 and tripped the task watchdog (panic, then reboot); and any
     * WiFi interrupt landing inside the 20 us SCK settle delay below stretched
     * that delay, degrading the bit timing of the very read being sampled. */
    int64_t deadline_us = esp_timer_get_time() + HX711_READY_TIMEOUT_US;
    int spins = 0;
    while (data_high(dev)) {
        if (esp_timer_get_time() > deadline_us) {
            return false;
        }
        /* Short bounded spin first, because after the 25th pulse the chip needs
         * most of a conversion period, so the common case is a long wait.
         *
         * The long wait must BLOCK, not taskYIELD(). Yield only switches to a
         * task of equal or higher priority, and the idle tasks sit at priority 0,
         * so yielding here keeps CPU 0 permanently busy: measured, this starved
         * IDLE0 and tripped the task watchdog every 5 s with
         * "CPU 0: main". Blocking hands the core back and lets idle feed the
         * watchdog. The 1 ms tick is well inside the conversion period and only
         * sets when the read starts, never the bit timing of the read itself. */
        if (++spins < 16) {
            taskYIELD();
        } else {
            vTaskDelay(1);
        }
    }

    if (wait_us != NULL) {
        *wait_us = esp_timer_get_time() - wait_start;
    }

    /* Mask interrupts for the bit-bang, and keep the mask on until the 25th pulse
     * has been emitted.
     *
     * This is the actual fix for the corrupted readings, and the reason is worth
     * recording because it is not obvious. esp_rom_delay_us() is a calibrated
     * busy loop, not a hardware timer, so any ISR that preempts it simply makes
     * the delay longer. The SCK period therefore became unpredictable, the chip
     * lost count of its own pulses, and reads came back as -1 or 0x3FFFFF.
     * Measured on an empty platform with the web page polling: 10.3% of samples
     * corrupted, and the displayed weight wandered over -0.16..+0.69 g. With the
     * radio off the same code gave sigma = 45.0 counts, exactly as designed, so
     * the timing - not the mechanics or the load cell - was at fault.
     *
     * The mask covers only the bit-bang itself, never the wait for DOUT above. */
    int64_t masked_at = esp_timer_get_time();
    if (irq_guard) {
        portENTER_CRITICAL(&dev->mux);
    }

    int32_t value = 0;
    for (int bit = 0; bit < HX711_DATA_BITS; bit++) {
        clock_high(dev);
        esp_rom_delay_us(HX711_SETTLE_US);
        value = (value << 1) | (data_high(dev) ? 1 : 0);
        clock_low(dev);
        esp_rom_delay_us(HX711_SETTLE_US);
    }

    /* Sign-extend from 24 to 32 bits. */
    if (value & (1 << 23)) {
        value |= (int32_t)(0xFF << 24);
    }

    /* The 25th pulse pulls DOUT high again and is what selects ch A / gain 128
     * for the NEXT conversion. Skipping it leaves the chip in the data-read
     * state with DOUT still low, so the next call sees "ready" immediately and
     * clocks out stale bits instead of a fresh sample.
     *
     * Total pulses per conversion must stay in 25-27; emitting more causes a
     * serial communication error. */
    clock_high(dev);
    esp_rom_delay_us(HX711_SETTLE_US);
    clock_low(dev);

    if (irq_guard) {
        portEXIT_CRITICAL(&dev->mux);
    }

    /* How long interrupts were actually off. Estimated from the datasheet as
     * 24 bits x 2 edges x 5 us = 240 us, but that is arithmetic, not a
     * measurement, and the whole design rests on this window being short. */
    int64_t span = esp_timer_get_time() - masked_at;
    if (span > hx711_mask_max_us) {
        hx711_mask_max_us = span;
    }
    hx711_mask_last_us = span;

    *raw = value;
    return true;
}