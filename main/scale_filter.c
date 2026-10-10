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
/* Auto-zero must be able to FOLLOW the platform, and to do that it needs a
 * window wider than the deadband.
 *
 * These used to be the same constant, DEADBAND was defined as AZT_WINDOW. That
 * made the tracking branch unreachable: it asks for |diff| <= AZT_WINDOW, but
 * the hard-zero branch above it already claims everything <= DEADBAND, and the
 * two numbers were equal. So the anchor could only ever snap to a reading that
 * was already inside the deadband, and once the platform wandered further than
 * that nothing pulled it back.
 *
 * Observed 2026-10-10 as "zero holds for a moment and then the figure walks
 * away and never comes back" - which is exactly the missing tracking.
 *
 * Window sized from measurement, not taste. Ten minutes with the platform
 * untouched: sigma 44 counts, peak to peak 258 counts. 400 counts is about nine
 * sigma and covers the whole observed range with margin.
 *
 * The cost, stated plainly: a load smaller than the window would eventually be
 * walked down to zero. 400 counts is 0.94 g. The smallest reference in use is
 * 30 g, 32 times larger, and a step of that size leaves the window immediately
 * so tracking never starts. AZT_HOLD_US guards the edge: the anchor only moves
 * after the reading has stayed inside the window for two seconds. */
#define AZT_WINDOW       400.0  /* counts, ~0.94 g */
#define AZT_RATE         30.0   /* counts/s: cross the window in ~13 s */
#define AZT_HOLD_US      2000000LL

/* Inside this the reading is forced to exactly zero, so the last digit cannot
 * flicker. Much narrower than AZT_WINDOW on purpose: it decides what counts as
 * "nothing there", while the window above decides how far the zero is allowed
 * to chase the platform. */
#define DEADBAND         40.0   /* counts, ~0.094 g */

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
            /* Exactly zero while inside the deadband, so the last digit cannot
             * flicker. The anchor is parked on the reading, and the wider window
             * below is what lets it follow the platform once the reading steps
             * outside. */
            f->azt_anchor = f->ema;
            f->azt_since_us = now_us;
        } else if (fabs(diff) <= AZT_WINDOW) {
            if (f->azt_since_us == 0) {
                f->azt_since_us = now_us;
            } else if (now_us - f->azt_since_us >= AZT_HOLD_US) {
                double dt = (double)(now_us - f->azt_since_us) / 1e6;
                /* Move the anchor TOWARD the reading.
                 *
                 * This used to be an unconditional `azt_anchor += AZT_RATE * dt`,
                 * which walks the zero upward no matter which side of the anchor
                 * the reading sits on. On a platform drifting down that pushes
                 * the zero the wrong way, so the figure walked away instead of
                 * coming back. The sign has to follow the reading. */
                double step = AZT_RATE * dt;
                f->azt_anchor += (f->ema > f->azt_anchor ? step : -step);
                /* Stop on the reading rather than overshooting it. The old clamp
                 * stopped at the window edge, which with a 400 count window would
                 * park the display at 0.94 g instead of at zero. */
                if ((diff > 0 && f->ema < f->azt_anchor) ||
                    (diff < 0 && f->ema > f->azt_anchor)) {
                    f->azt_anchor = f->ema;
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