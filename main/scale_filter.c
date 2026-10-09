/*
 * scale_filter.c - see scale_filter.h for the derivation of every threshold.
 */
#include "scale_filter.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"

/* Measured sigma of a raw sample at 11.3 Hz. */
#define SIGMA_RAW        45.0
/* median(5) leaves roughly 85% of the Gaussian sigma. */
#define SIGMA_MED        (SIGMA_RAW * 0.85)

#define ALPHA_STEADY     0.10   /* at rest: ~9 counts sigma = ~22 mg */
#define ALPHA_FAST       0.80   /* weight moving */
#define DELTA_STATIC     (2.0 * SIGMA_MED)    /* below this, at rest */
#define DELTA_DYNAMIC    (10.0 * SIGMA_MED)   /* above this, clearly moving */

/* Auto-zero. The window is deliberately modest: drift reaches tens of counts per
 * capture, but a wide window would swallow real small loads. At the 0.05 g
 * target the practical floor is ~0.1 g, and AZT is a convenience for holding
 * zero, not a substitute for the TARE button. */
#define AZT_WINDOW       40.0   /* counts, ~0.094 g */
#define AZT_RATE         6.0    /* counts/s, must beat the worst drift of 6.1 */
#define AZT_HOLD_US      2000000LL

/* Deadband equals the AZT window so zero does not flicker between 0 and a
 * non-zero digit when the two disagree. */
#define DEADBAND         AZT_WINDOW

/* STABLE is computed on RAW samples before any smoothing. On the EMA output a
 * smoothed signal is trivially always "stable" and the indicator is useless. */
#define STABLE_WINDOW    10
#define STABLE_SIGMA     (2.0 * SIGMA_RAW)

#define MEDIAN_N         5

static double median_of(const int32_t *v, int n)
{
    int32_t tmp[MEDIAN_N];
    memcpy(tmp, v, sizeof(int32_t) * n);
    for (int i = 1; i < n; i++) {
        int32_t key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    if (n % 2) {
        return tmp[n / 2];
    }
    return 0.5 * ((double)tmp[n / 2 - 1] + tmp[n / 2]);
}

static double stdev_of(const int32_t *v, int n)
{
    if (n < 2) {
        return 0.0;
    }
    double mean = 0.0;
    for (int i = 0; i < n; i++) {
        mean += v[i];
    }
    mean /= n;
    double ss = 0.0;
    for (int i = 0; i < n; i++) {
        double d = v[i] - mean;
        ss += d * d;
    }
    return sqrt(ss / (n - 1));
}

void scale_filter_init(scale_filter_t *f, float cal_factor, int32_t tare_offset)
{
    memset(f, 0, sizeof(*f));
    f->cal_factor = cal_factor;
    f->tare_offset = tare_offset;
    f->azt_anchor = tare_offset;
    f->azt_enabled = true;
    f->unit = UNIT_GRAM;
}

void scale_filter_update(scale_filter_t *f, int32_t raw, int64_t now_us,
                         scale_reading_t *out)
{
    memset(out, 0, sizeof(*out));
    out->counts = raw;

    /* ---- median window ---- */
    f->median_hist[f->median_idx] = raw;
    f->median_idx = (f->median_idx + 1) % MEDIAN_N;
    if (f->median_len < MEDIAN_N) {
        f->median_len++;
    }

    /* ---- raw window for STABLE ---- */
    f->raw_hist[f->raw_idx] = raw;
    f->raw_idx = (f->raw_idx + 1) % STABLE_WINDOW;
    if (f->raw_len < STABLE_WINDOW) {
        f->raw_len++;
    }

    if (f->median_len < MEDIAN_N) {
        out->raw_grams = 0.0f;
        out->grams = 0.0f;
        return;
    }

    double med = median_of(f->median_hist, MEDIAN_N);

    /* ---- adaptive EMA ---- */
    if (!f->ema_valid) {
        f->ema = med;
        f->ema_valid = true;
        out->alpha = ALPHA_FAST;
    } else {
        double delta = fabs(med - f->ema);
        double alpha;
        if (delta <= DELTA_STATIC) {
            alpha = ALPHA_STEADY;
        } else if (delta >= DELTA_DYNAMIC) {
            alpha = ALPHA_FAST;
        } else {
            double frac = (delta - DELTA_STATIC) / (DELTA_DYNAMIC - DELTA_STATIC);
            alpha = ALPHA_STEADY + frac * (ALPHA_FAST - ALPHA_STEADY);
        }
        f->ema += alpha * (med - f->ema);
        out->alpha = (float)alpha;
    }

    /* ---- auto-zero tracking, in counts per second ----
     * Rate based rather than a fixed fraction: a fixed fraction either tracks too
     * slowly to beat the drift or too aggressively to preserve real weights. */
    if (f->azt_enabled) {
        double diff = f->ema - f->azt_anchor;

        if (fabs(diff) <= DEADBAND) {
            /* Exactly zero while inside the deadband.
             *
             * The old code clamped the anchor to AZT_WINDOW when it overtook the
             * reading, and AZT_WINDOW is the SAME constant as DEADBAND. So the
             * anchor was parked precisely where the zero/non-zero decision is
             * taken: 40.000000 counts reads as zero, 40.000001 reads as 0.09 g.
             * Measured consequence - an empty platform sitting permanently at
             * -0.10 g, with the ZERO indicator off and nothing on the scale.
             *
             * Snapping to the reading inside the deadband removes the stuck
             * state entirely: the zero is exact, and a real load still moves the
             * reading far outside the deadband so tracking resumes normally. */
            f->azt_anchor = f->ema;
            f->azt_since_us = now_us;
        } else if (fabs(diff) <= AZT_WINDOW) {
            if (f->azt_since_us == 0) {
                f->azt_since_us = now_us;
            } else if (now_us - f->azt_since_us >= AZT_HOLD_US) {
                double dt = (double)(now_us - f->azt_since_us) / 1e6;
                f->azt_anchor += AZT_RATE * dt;
                if (fabs(f->ema - f->azt_anchor) > AZT_WINDOW) {
                    /* never let the anchor run past the reading it chases */
                    f->azt_anchor = f->ema - (f->ema > f->azt_anchor
                                              ? AZT_WINDOW : -AZT_WINDOW);
                }
                f->azt_since_us = now_us;
            }
        } else {
            f->azt_since_us = 0;
        }
    }

    /* ---- to grams ---- */
    double counts = f->ema - f->azt_anchor;
    double grams = counts / f->cal_factor;
    out->raw_grams = (float)grams;

    if (fabs(counts) <= DEADBAND) {
        out->grams = 0.0f;
        out->zero = true;
    } else {
        out->grams = (float)grams;
    }

    /* ---- STABLE, from raw samples only ---- */
    if (f->raw_len >= STABLE_WINDOW) {
        out->stable = stdev_of(f->raw_hist, STABLE_WINDOW) < STABLE_SIGMA;
    }

    out->tare_active = f->tare_armed;
}

void scale_filter_tare(scale_filter_t *f, bool hard)
{
    if (hard && f->ema_valid) {
        f->tare_offset = (int32_t)llround(f->ema);
        f->tare_armed = true;
    }
    /* Both paths re-anchor: the goal is that the display reads zero now, whether
     * by storing the offset or by letting AZT pull the anchor across. */
    f->azt_anchor = f->ema_valid ? f->ema : (double)f->tare_offset;
    f->azt_since_us = 0;
    f->azt_enabled = true;
}

void scale_filter_set_unit(scale_filter_t *f, scale_unit_t unit)
{
    f->unit = unit;
}

void scale_filter_set_calibration(scale_filter_t *f, float cal, int32_t tare)
{
    if (cal > 0.0f) {
        f->cal_factor = cal;
    }
    f->tare_offset = tare;
    f->azt_anchor = (double)tare;
    f->azt_since_us = 0;
}

float scale_filter_get_cal_factor(const scale_filter_t *f)
{
    return f->cal_factor;
}

int32_t scale_filter_get_tare(const scale_filter_t *f)
{
    return f->tare_offset;
}

double scale_convert_units(const scale_filter_t *f, float grams)
{
    switch (f->unit) {
    case UNIT_KILOGRAM:
        return grams / 1000.0;
    case UNIT_OUNCE:
        return grams / 28.349523125;
    case UNIT_GRAM:
    default:
        return grams;
    }
}