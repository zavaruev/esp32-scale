/*
 * hx711.h - bit-banged driver for the HX711 24-bit sigma-delta ADC
 *
 * Wiring (ESP32-S3 side, per hardware spec):
 *   DT  (DATA) -> GPIO16 (input)
 *   SCK (CLK)  -> GPIO17 (output)
 *
 * IMPORTANT: the HX711 drives its DOUT pin at its own VCC level. Running the
 * chip from 3.3 V is what keeps DOUT at 3.3 V, which is the only level the
 * ESP32-S3 input tolerates.
 *
 * OUTPUT RATE IS NOT SOFTWARE-CONTROLLABLE on this part. It is set by the level
 * on physical pin 15 (RATE): 0 = 10 Hz, 1 = 80 Hz. Modules generally strap RATE
 * low, which is why this driver offers no rate API. Getting 80 SPS means wiring
 * that pin - see docs/stage1-findings.md.
 */
#ifndef HX711_H
#define HX711_H

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    gpio_num_t data_pin;
    gpio_num_t clock_pin;
    /* Guards the bit-bang. It is held only for the ~250 us of the read itself,
     * with interrupts masked, and never across the wait for a conversion. */
    portMUX_TYPE mux;
} hx711_t;

/* Configures pins. SCK is driven LOW and held there, DOUT becomes a pulled-down
 * input. That order matters: if DOUT is released to high-Z while SCK is high,
 * a phantom edge can appear on the data line. */
void hx711_init(hx711_t *dev, gpio_num_t data_pin, gpio_num_t clock_pin);

/* Reads one 24-bit signed conversion result, channel A at gain 128.
 * Returns false only if the chip never signalled "conversion ready" in time. */
/* On success writes the 24-bit result and, if wait_us is non-NULL, how long the
 * DOUT line took to assert. The wait time is the single most useful number for
 * telling a healthy read from one that is about to time out. */
bool hx711_read_raw(hx711_t *dev, int32_t *raw, int64_t *wait_us);

/* Enables/disables the interrupt mask around the bit-bang. Runtime toggle so the
 * effect on the WiFi driver can be A/B tested in one build instead of guessing. */
void hx711_set_irq_guard(bool on);

/* Actual, not estimated, length of the interrupt-masked window in microseconds. */
extern int64_t hx711_mask_last_us;
extern int64_t hx711_mask_max_us;

#ifdef __cplusplus
}
#endif

#endif /* HX711_H */