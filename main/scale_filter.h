/*
 * scale_filter.h - signal chain: median -> adaptive EMA -> AZT -> deadband.
 *
 * This is the C port of tools/filter_sim.py, which was validated against real
 * captures before being written here. Thresholds are in COUNTS and derived from
 * the measured sigma, not copied from the specification - the spec's values were
 * written for 0.01 g resolution and are far too tight at our 0.05 g target.
 *
 * Measured basis (2026-10-07):
 *   raw sigma     43-48 counts, sample rate 11.3 Hz (fixed by HX711 pin 15)
 *   cal_factor    425.6 counts/g, tare_offset 418857
 */
#ifndef SCALE_FILTER_H
#define SCALE_FILTER_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    UNIT_GRAM = 0,
    UNIT_KILOGRAM,
    UNIT_OUNCE,
} scale_unit_t;

typedef struct {
    float alpha;
    float grams;        /* deadband applied */
    float raw_grams;    /* before deadband */
    bool  stable;
    bool  zero;
    bool  tare_active;
    int32_t counts;     /* raw ADC count, for diagnostics */
} scale_reading_t;

typedef struct {
    /* calibration, persisted in NVS */
    float   cal_factor;
    int32_t tare_offset;

    /* filter state */
    int32_t median_hist[5];
    int     median_len;
    int     median_idx;
    double  ema;
    bool    ema_valid;
    int32_t raw_hist[10];
    int     raw_len;
    int     raw_idx;

    /* auto-zero tracking */
    double  azt_anchor;      /* tare currently being tracked toward */
    int64_t azt_since_us;
    bool    azt_enabled;

    scale_unit_t unit;
    bool    tare_armed;
} scale_filter_t;

void scale_filter_init(scale_filter_t *f, float cal_factor, int32_t tare_offset);

/* Feeds one raw sample and produces a reading. */
void scale_filter_update(scale_filter_t *f, int32_t raw, int64_t now_us,
                         scale_reading_t *out);

/* Sets the tare to the current filtered value (TARE button). Passing false
 * re-arms auto-zero only. */
void scale_filter_tare(scale_filter_t *f, bool hard);

void scale_filter_set_unit(scale_filter_t *f, scale_unit_t unit);
void scale_filter_set_calibration(scale_filter_t *f, float cal, int32_t tare);
float scale_filter_get_cal_factor(const scale_filter_t *f);
int32_t scale_filter_get_tare(const scale_filter_t *f);

/* Converts grams to the currently selected unit for display. */
double scale_convert_units(const scale_filter_t *f, float grams);

#endif /* SCALE_FILTER_H */