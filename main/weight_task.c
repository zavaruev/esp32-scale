/*
 * weight_task.c - core 0: acquire, filter, publish.
 */
#include "weight_task.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hx711.h"
#include "idle_tare.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "weight";

#define HX711_DATA_GPIO GPIO_NUM_16
#define HX711_CLOCK_GPIO GPIO_NUM_17

#define NVS_NS      "scale"
#define NVS_KEY_CAL "cal_factor"
#define NVS_KEY_TARE "tare_offset"
#define NVS_KEY_UNIT "unit"
#define NVS_KEY_REF_MASS "ref_mass"   /* reference weight for panel calibration */
/* ~4.5 s of warm-up before the boot tare is taken. */
#define BOOT_TARE_SETTLE_SAMPLES 51
/* How far the platform may sit from the confirmed tare and still count as
 * empty. ~2 g at 425.6 counts per gram. The resting point wanders about 0.6 g
 * peak to peak, so this is about eight times the noise: an empty platform is
 * never mistaken for a load, and every real reference is. */
#define BOOT_TARE_TOLERANCE_COUNTS 851

/* Idle zero autosave. The rest point walks ~2 g/hour; refreshing the stored
 * tare every 30 min keeps it inside the 2 g boot-tare tolerance. One write
 * every 30 min is negligible for NVS wear. */
#define IDLE_TARE_SAVE_US 1800000000LL

/* What NVS held at start-up, kept so the boot tare can tell an empty platform
 * from a loaded one. */
static int32_t nvs_tare_counts;
static bool have_nvs_tare;

/* Mass of the reference used by the one-touch calibration on the front panel.
 * Set once from the web interface or the `F` console command; 73.03 g is the
 * reference already used for the paired measurements in AGENTS.md. */
static float ref_mass = 73.03f;

#define NVS_MAGIC   0xAA55BEEF
#define NVS_KEY_MAGIC "magic"

static scale_filter_t filter;
static SemaphoreHandle_t lock;
static scale_snapshot_t snapshot;
static volatile bool tare_pending = false;

/* Mean of the raw counts over a window whose LENGTH IS NOT FIXED.
 *
 * Measured on a real step (logs/coins2.log, a 30.6 g reference placed on a
 * platform already under measurement), time from the step to within 0.05 g:
 *
 *     window 45  4.35 s   final sigma 11 mg
 *     window 18  2.40 s   final sigma 23 mg
 *     window  9  1.15 s   final sigma 36 mg
 *
 * A boxcar mean takes N samples to absorb a step of any size - it has to flush
 * the old samples out - so N is the response time, and it is not a trade-off
 * that a cleverer formula can dodge.
 *
 * The fixed window of 45 was therefore buying precision the display cannot
 * show: the step is 0.05 g = 50 mg, and even a window of 9 settles to 36 mg,
 * already inside one step. Meanwhile it cost 3.2 s on every single reading.
 *
 * So the length follows the situation. Short while the reading is still
 * converging, where the only thing that matters is that the figure arrives;
 * grown back to the full window once it has arrived, which is where the low
 * noise is wanted. The test is whether the mean itself has stopped moving -
 * see the note at the point of use for why keying off the EMA's alpha was
 * wrong and measurably slower.
 *
 * The window grows by one sample per reading rather than jumping, so the mean
 * never steps when the length changes - a sudden change of length would move the
 * estimate by the difference between the two window noises, which is visible. */
#define AVG_WINDOW_SAMPLES 45      /* ~4.0 s at 11.3 Hz, the accuracy end */
#define AVG_WINDOW_MIN 9           /* ~0.8 s, the response end */
/* How far the mean may move between two samples before the window is held
 * short. At rest with the short window that movement is about 21 counts, so
 * this is five times it; during the settling of a 73 g step it is 385. */
#define AVG_STEP_COUNTS 100.0
static int32_t avg_hist[AVG_WINDOW_SAMPLES];
static int avg_idx;
static int avg_filled;             /* valid entries in the ring, up to 45 */
static int avg_win = AVG_WINDOW_MIN;  /* entries the mean actually covers */
static double avg_prev_counts;     /* the mean at the previous sample */
static bool avg_have_prev;

static float sigma_est = 45.0f;   /* EWMA of raw sigma, for diagnostics */

/* --------------------------------------------------------------- NVS helpers */

static bool nvs_load(scale_filter_t *f)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "NVS empty, using built-in defaults");
        return false;
    }
    uint32_t magic = 0;
    nvs_get_u32(h, NVS_KEY_MAGIC, &magic);
    if (magic != NVS_MAGIC) {
        ESP_LOGW(TAG, "magic mismatch (%08x), using defaults", (unsigned)magic);
        nvs_close(h);
        return false;
    }
    float cal = 0.0f;
    int32_t tare = 0;
    uint8_t unit = 0;
    nvs_get_i32(h, NVS_KEY_TARE, &tare);
    nvs_get_u8(h, NVS_KEY_UNIT, &unit);

    size_t ref_len = sizeof(ref_mass);
    float stored_ref = 0.0f;
    if (nvs_get_blob(h, NVS_KEY_REF_MASS, &stored_ref, &ref_len) == ESP_OK &&
        ref_len == sizeof(stored_ref) && stored_ref > 0.0f) {
        ref_mass = stored_ref;
    }

    /* Read the calibration back the way it was written.
     *
     * nvs_save stores cal_factor with nvs_set_blob, because it is a float and the
     * integer encodings of set_i32 are not worth relying on. This used to read it
     * with nvs_get_i32, which fails on a blob-typed key with a type mismatch, so
     * every boot logged "cal_factor missing or invalid" and fell back to the
     * built-in default. The effect was silent and bad: a calibration performed
     * with the m command was written, believed, and then thrown away on the next
     * reboot, with nothing on screen to say so.
     *
     * The i32 path is kept as a fallback in case a partition was ever written by
     * different code, but the blob is what this firmware has always written. */
    size_t cal_len = sizeof(cal);
    if (nvs_get_blob(h, NVS_KEY_CAL, &cal, &cal_len) != ESP_OK ||
        cal_len != sizeof(cal) || cal <= 0.0f) {
        int32_t cal_i32 = 0;
        if (nvs_get_i32(h, NVS_KEY_CAL, &cal_i32) == ESP_OK && cal_i32 > 0) {
            cal = (float)cal_i32;
            ESP_LOGW(TAG, "cal_factor stored as integer, recovered %d",
                     cal_i32);
        } else {
            ESP_LOGW(TAG, "cal_factor missing or invalid, keeping default");
            cal = 0.0f;
        }
    }
    nvs_close(h);

    if (cal > 0.0f) {
        f->cal_factor = cal;
    }
    f->tare_offset = tare;
    /* The auto-zero anchor has to follow the tare, not the built-in default.
     *
     * scale_filter_init sets azt_anchor from the value passed to it, and the task
     * passes the hard-coded 418857. Loading the real tare from NVS without moving
     * the anchor left every reading on the boot biased by the difference between
     * the two, and that difference grows as the stored tare drifts. Measured here:
     * anchor 418878 against a stored tare of 419002, an offset of 124 counts,
     * 293 mg, silently added to every weight shown.
     *
     * Verified afterwards by recomputing the anchor from the API as
     * counts - value*cal_factor: it must land on the stored tare. */
    f->azt_anchor = (double)tare;
    f->azt_since_us = 0;
    /* Legacy NVS values 0/1 (g/kg) become AUTO; 2 stays ounces. */
    f->unit = (unit == 2) ? UNIT_OUNCE : UNIT_AUTO;
    /* Kept so the boot tare can tell an empty platform from a loaded one. See
     * the block in the measurement task. */
    nvs_tare_counts = tare;
    have_nvs_tare = true;
    ESP_LOGI(TAG, "loaded cal=%.2f tare=%d anchor=%d", (double)f->cal_factor,
             (int)f->tare_offset, (int)f->azt_anchor);
    return true;
}

static esp_err_t nvs_save(const scale_filter_t *f)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    /* write_blob on a float avoids the integer-encoding surprises of set_i32 on a
     * float value, and keeps the struct on-disk layout explicit. */
    float cal = f->cal_factor;
    nvs_set_blob(h, NVS_KEY_CAL, &cal, sizeof(cal));
    nvs_set_i32(h, NVS_KEY_TARE, f->tare_offset);
    nvs_set_u8(h, NVS_KEY_UNIT, (uint8_t)f->unit);
    nvs_set_blob(h, NVS_KEY_REF_MASS, &ref_mass, sizeof(ref_mass));
    nvs_set_u32(h, NVS_KEY_MAGIC, NVS_MAGIC);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* -------------------------------------------------------------- measurement */

/* Diagnostics ring. Only the measurement task writes it, and only inside the
 * same critical section that publishes the snapshot. See weight_task.h for why
 * the console must not poll for samples instead. */
static raw_log_entry_t rawlog[RAW_LOG_N];
static int rawlog_idx;
static int rawlog_len;
static uint32_t raw_seq;
static uint32_t read_failures;
static uint32_t total_read_failures;
static int64_t last_wait_us;
static idle_tare_t idle_tare = { .period_us = IDLE_TARE_SAVE_US };

static void measure_task(void *arg)
{
    (void)arg;
    hx711_t dev;
    hx711_init(&dev, HX711_DATA_GPIO, HX711_CLOCK_GPIO);

    scale_reading_t r;
    double prev_ema = 0.0;
    bool have_prev = false;
    int settle_count = 0;

    for (;;) {
        int64_t wait_us = 0;
        int64_t now_read = esp_timer_get_time();
        int32_t raw = 0;
        if (!hx711_read_raw(&dev, &raw, &wait_us)) {
            /* Back off on failure. Continuing immediately turns a DOUT line that
             * stays high - unpowered module, or a missed 25th pulse - into a
             * 100% CPU 0 spin at priority 5: IDLE0 never runs, the task watchdog
             * panics and the board reboots. Degrading to "no new samples" plus a
             * warning is the honest behaviour for a failed read. */
            read_failures++;
            total_read_failures++;
            if (read_failures == 1 || read_failures % 100 == 0) {
                ESP_LOGW(TAG, "HX711 not ready, consecutive failures %u",
                         (unsigned)read_failures);
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        read_failures = 0;
        last_wait_us = wait_us;
        int64_t now = esp_timer_get_time();
        scale_filter_update(&filter, raw, now, &r);

        /* Track sigma on raw samples for the diagnostics panel. */
        if (have_prev) {
            double d = fabs(raw - prev_ema);
            if (d > 0.0 && d < 10000.0) {
                sigma_est += 0.02f * (float)(d * 1.2533 - sigma_est);
            }
        }
        prev_ema = raw;
        have_prev = true;

        /* Auto-tare at boot, once the filter has settled.
         *
         * With external power there is no USB console to fall back on, so an
         * un-tared display is unrecoverable without reflashing. The spec asks for
         * boot auto-tare anyway. It waits for the median window to fill and then
         * a settle period, because taring mid-warm-up would capture drift.
         *
         * But it must not tare a loaded platform. Observed 2026-10-10: with a
         * 73 g reference sitting on the scale, a reboot tared the loaded level
         * (450 023 counts) and the scale read a confident 0.000 g. A silent
         * wrong number is far worse than a visibly wrong one - the user cannot
         * see a zero that should have been 73, but they can see a -9.8 g that
         * asks for a tare. So the boot tare only runs when the platform reads
         * close to the tare the user last confirmed.
         *
         * Measured spread of the resting point with the platform untouched:
         * about 0.6 g peak to peak, roughly 256 counts. A tolerance of 2 g is
         * about eight times that, so a genuine empty platform is never mistaken
         * for a load, and anything heavier than 2 g - which includes every real
         * reference - is left alone.
         *
         * With nothing stored in NVS at all there is nothing to compare against,
         * and the display would be meaningless, so it tares unconditionally. */
        if (tare_pending && filter.ema_valid) {
            settle_count++;
            if (settle_count > BOOT_TARE_SETTLE_SAMPLES) {
                tare_pending = false;
                xSemaphoreTake(lock, portMAX_DELAY);
                /* Compared against the EMA, not the raw count: scale_filter_tare
                 * stores llround(f->ema), so the tare lives in EMA units. */
                int32_t nvs_tare = nvs_tare_counts;
                long drift = lround(filter.ema) - (long)nvs_tare;
                if (!have_nvs_tare) {
                    /* First run or erased flash: nothing to compare against. */
                    scale_filter_tare(&filter, true);
                    ESP_LOGW(TAG, "no stored tare, boot tared at %ld counts",
                             (long)filter.tare_offset);
                } else if (drift > -BOOT_TARE_TOLERANCE_COUNTS &&
                           drift < BOOT_TARE_TOLERANCE_COUNTS) {
                    /* Deliberately NOT saved to NVS. Writing on every boot would
                     * burn the flash partition and would also make the stored
                     * tare drift silently over time. TARE in memory is enough:
                     * the stored value stays as the last the user confirmed. */
                    scale_filter_tare(&filter, true);
                    ESP_LOGI(TAG, "boot auto-tare %ld counts off the stored "
                                  "tare, taken at %ld (NVS keeps %ld)",
                             drift, (long)filter.tare_offset, (long)nvs_tare);
                } else {
                    ESP_LOGW(TAG, "NOT taring at boot: platform reads %ld counts "
                                  "from the stored tare %ld, which is %.0f g - "
                                  "looks like a load is on it. Display will show "
                                  "that load; press TARE once it is removed.",
                             drift, (long)nvs_tare,
                             (double)drift / filter.cal_factor);
                }
                xSemaphoreGive(lock);
            }
        }

        if (xSemaphoreTake(lock, portMAX_DELAY) == pdTRUE) {
            /* Running mean of the raw stream, converted against the same anchor
             * the live figure uses so tare and the zero flag behave identically. */
            /* Progressive window: short while the reading is still converging, long once it
             * has arrived. See the note on the constants for the measurement.
             *
             * The test is whether THE MEAN has stopped moving, not whether the EMA
             * has. Keying off the EMA's alpha was wrong and measurably so: after a
             * step the alpha drops within a couple of samples while the mean is
             * still 30 g short, so the window began growing straight away and then
             * held the pre-step samples in for longer than it needed to. Measured
             * on the device: a 73 g placement climbed from 36 g to 73 g over 3.6 s
             * with the window growing 9, 11, 14, 19, 24, 31, 40, 45 through it.
             *
             * Comparing the mean against its own previous value is what "the
             * reading has settled" actually means here, and it costs one double.
             *
             * The threshold is set from the noise, not by taste. At rest with a
             * window of 9 the mean moves about 21 counts between samples
             * (sigma_raw * sqrt(2) / sqrt(9)); 100 counts is roughly five of those,
             * while the rise during a settling of 73 g is 385 counts per sample. */
            avg_hist[avg_idx] = raw;
            avg_idx = (avg_idx + 1) % AVG_WINDOW_SAMPLES;
            if (avg_filled < AVG_WINDOW_SAMPLES) {
                avg_filled++;
            }

            /* Summed over the requested window rather than kept running. The
             * window length changes, and a running sum cannot follow a length
             * change without knowing what left it; re-adding at most 45 numbers
             * 11 times a second costs nothing and removes that bookkeeping. */
            int n = avg_win < avg_filled ? avg_win : avg_filled;
            int64_t sum = 0;
            for (int k = 1; k <= n; k++) {
                int j = avg_idx - k;
                if (j < 0) j += AVG_WINDOW_SAMPLES;
                sum += avg_hist[j];
            }
            double avg_counts = (double)sum / n;

            if (avg_have_prev && fabs(avg_counts - avg_prev_counts) > AVG_STEP_COUNTS) {
                avg_win = AVG_WINDOW_MIN;          /* still travelling */
            } else if (avg_win < AVG_WINDOW_SAMPLES) {
                avg_win++;                          /* arrived; earn the accuracy */
            }
            avg_prev_counts = avg_counts;
            avg_have_prev = true;

            double avg_rel = avg_counts - filter.azt_anchor;
            snapshot.avg_grams = (float)(avg_rel / filter.cal_factor);
            snapshot.avg_n = (uint16_t)n;
            snapshot.avg_sigma_mg = (float)(sigma_est /
                                            sqrt((double)n) /
                                            filter.cal_factor * 1000.0);

            snapshot.grams = r.grams;
            if (filter.unit == UNIT_AUTO) {
                snapshot.effective_unit = (fabsf(snapshot.avg_grams) >= 1000.0f)
                                          ? UNIT_KILOGRAM : UNIT_GRAM;
                snapshot.display_value =
                    (float)scale_convert_units(&filter, snapshot.avg_grams);
            } else {
                snapshot.effective_unit = filter.unit;
                snapshot.display_value = (float)scale_convert_units(&filter, r.grams);
            }
            snapshot.sigma_mg = sigma_est / filter.cal_factor * 1000.0f;
            snapshot.stable = r.stable;
            snapshot.zero = r.zero;
            snapshot.tare_active = r.tare_active;
            snapshot.azt_enabled = filter.azt_enabled;
            snapshot.counts = r.counts;
            snapshot.cal_factor = filter.cal_factor;
            snapshot.tare_offset = filter.tare_offset;
            snapshot.unit = filter.unit;
            snapshot.uptime_s = (uint32_t)(now / 1000000);
            snapshot.sample_seq++;
            snapshot.updated = true;

            /* Follow the platform's rest point while the scale is idle. AZT
             * holds the display at zero in RAM, but the value in NVS would
             * otherwise stay at the last manual tare; after a few hours of
             * drift the boot tare refuses it (2 g tolerance) and the empty
             * platform reads as a load. Save only when the instrument itself
             * says zero - a real load must never be swallowed silently. */
            if (idle_tare_due(&idle_tare, now,
                              filter.ema_valid && snapshot.stable,
                              snapshot.zero)) {
                int32_t idle_counts = (int32_t)llround(filter.ema);
                if (idle_counts != filter.tare_offset) {
                    int32_t old_tare = filter.tare_offset;
                    /* Not scale_filter_tare(): that would light the TARE
                     * indicator. Only the stored reference moves. */
                    filter.tare_offset = idle_counts;
                    nvs_save(&filter);
                    ESP_LOGI(TAG, "idle tare saved: %ld counts (was %ld)",
                             (long)idle_counts, (long)old_tare);
                }
            }

            rawlog[rawlog_idx].seq = ++raw_seq;
            rawlog[rawlog_idx].t_us = now_read;
            rawlog[rawlog_idx].counts = raw;
            rawlog_idx = (rawlog_idx + 1) % RAW_LOG_N;
            if (rawlog_len < RAW_LOG_N) {
                rawlog_len++;
            }
            xSemaphoreGive(lock);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t weight_task_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    scale_filter_init(&filter, 425.6f, 418857);
    nvs_load(&filter);

    tare_pending = true;

    lock = xSemaphoreCreateMutex();
    if (lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    /* Core 0 owns acquisition, per the hardware spec. */
    if (xTaskCreatePinnedToCore(measure_task, "measure", 4096, NULL, 5, NULL,
                                0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "measurement task on core 0");
    return ESP_OK;
}

int weight_task_raw_log(raw_log_entry_t *out, int n)
{
    /* Copied straight into the caller's buffer. A staging buffer here doubled the
     * footprint and did not fit in dram0. */
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        return 0;
    }
    int have = rawlog_len;
    int start = (rawlog_len == RAW_LOG_N) ? rawlog_idx : 0;
    int k = have < n ? have : n;
    int skip = have - k;                 /* oldest first */
    for (int i = 0; i < k; i++) {
        out[i] = rawlog[(start + skip + i) % RAW_LOG_N];
    }
    xSemaphoreGive(lock);
    return k;
}

int weight_task_raw_log_count(void)
{
    return rawlog_len;
}

void weight_task_raw_log_clear(void)
{
    xSemaphoreTake(lock, pdMS_TO_TICKS(50));
    rawlog_idx = 0;
    rawlog_len = 0;
    xSemaphoreGive(lock);
}

uint32_t weight_task_read_failures(void)
{
    return total_read_failures;
}

int64_t weight_task_last_wait_us(void)
{
    return last_wait_us;
}


/* ----------------------------------------------------------- calibration */

static cal_state_t cal_state;
static SemaphoreHandle_t cal_lock;
static volatile bool cal_running = false;
/* Kept as a double rather than passed through the task argument: a task handle
 * argument is a void*, and routing 73.03 through intptr_t truncates it to 73. */
static double cal_mass_hint;

void weight_task_cal_get(cal_state_t *out)
{
    if (cal_lock == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(cal_lock, pdMS_TO_TICKS(50));
    *out = cal_state;
    xSemaphoreGive(cal_lock);
}

static void cal_publish(cal_phase_t phase, const char *detail)
{
    xSemaphoreTake(cal_lock, portMAX_DELAY);
    cal_state.phase = phase;
    snprintf(cal_state.detail, sizeof(cal_state.detail), "%s", detail);
    xSemaphoreGive(cal_lock);
}

static void cal_elapsed(int64_t t0)
{
    xSemaphoreTake(cal_lock, portMAX_DELAY);
    cal_state.elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    xSemaphoreGive(cal_lock);
}

/* A real reference sits thousands of counts above the tare; the worst measured
 * zero drift is 6 counts/s, so this cannot be triggered by noise. */
#define CAL_LOAD_DETECT_COUNTS 2000
#define CAL_SETTLE_MIN_MS      15000
#define CAL_SETTLE_HOLD_TICKS  20      /* 5 s of continuous STABLE */
#define CAL_SETTLE_MAX_MS      240000
#define CAL_WAIT_LOAD_MS       300000
#define CAL_AVERAGE_MS         20000

static void cal_task(void *arg)
{
    (void)arg;
    double mass_hint = cal_mass_hint;
    int64_t t0 = esp_timer_get_time();
    scale_snapshot_t snap;

    xSemaphoreTake(lock, portMAX_DELAY);
    bool azt_was_on = filter.azt_enabled;
    filter.azt_enabled = false;   /* AZT would swallow the true zero */
    xSemaphoreGive(lock);
    cal_publish(CAL_SETTLING, "platform must be EMPTY");

    /* Phase 1: wait until the empty platform genuinely stops moving.
     *
     * A fixed short wait is a race. Measured, the platform settled by ~1.8 g
     * within the first 30 s after boot or after anyone touched it, so an early
     * tare lands that whole error inside cal_factor. */
    int stable = 0;
    int32_t tare = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        weight_task_get(&snap);
        int64_t ms = (esp_timer_get_time() - t0) / 1000;
        cal_elapsed(t0);
        if (ms > CAL_SETTLE_MAX_MS) {
            break;
        }
        stable = snap.stable ? stable + 1 : 0;
        if (ms >= CAL_SETTLE_MIN_MS && stable >= CAL_SETTLE_HOLD_TICKS) {
            break;
        }
    }
    weight_task_get(&snap);
    tare = snap.counts;
    xSemaphoreTake(lock, portMAX_DELAY);
    scale_filter_tare(&filter, true);
    filter.azt_enabled = false;
    xSemaphoreGive(lock);
    xSemaphoreTake(cal_lock, portMAX_DELAY);
    cal_state.tare = tare;
    xSemaphoreGive(cal_lock);
    cal_publish(CAL_WAIT_LOAD, "put the reference on the platform");

    /* Phase 2: wait for the load. Auto-detected so the operator is not part of
     * the timing loop. */
    bool detected = false;
    int64_t load_at = 0;
    /* esp_timer_get_time() is microseconds while the timeout constants are in
     * milliseconds, so the comparison has to be scaled. Compared raw, 15 s read
     * as larger than 300000 and this loop exited the instant the tare was
     * taken - the phase died in under a second and reported "no load appeared". */
    while ((esp_timer_get_time() - t0) / 1000 < CAL_WAIT_LOAD_MS) {
        vTaskDelay(pdMS_TO_TICKS(250));
        weight_task_get(&snap);
        cal_elapsed(t0);

        /* While the platform is still empty, drag the anchor along with the
         * measured EMA. AZT has to stay off here - with a reference on the scale
         * it would happily eat the load - but leaving the anchor frozen for the
         * whole wait is what made the display wrong afterwards: the platform
         * drifts by ~0.4 g over a few minutes, and that error then applied to
         * whatever was weighed next. Measured, the reading sat 0.38 g under the
         * true mass. Tracking stops the moment a load appears. */
        xSemaphoreTake(lock, pdMS_TO_TICKS(50));
        if (!detected && filter.ema_valid) {
            filter.azt_anchor = filter.ema;
            filter.azt_since_us = 0;
        }
        xSemaphoreGive(lock);

        long jump = (long)snap.counts - tare;
        if (jump < 0) {
            jump = -jump;
        }
        if (jump > CAL_LOAD_DETECT_COUNTS) {
            detected = true;
            load_at = esp_timer_get_time();
            break;
        }
    }
    if (!detected) {
        cal_publish(CAL_ABORT, "no load appeared");
        goto restore;
    }

    /* Phase 3: settle, then average long enough to beat the noise. */
    cal_publish(CAL_MEASURING, "averaging");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* The averaging window starts HERE, not at the moment the load was detected.
     * Measured: computing it as load_at + 3 s meant the settle delay had already
     * consumed the whole window by the time it was tested, the loop never ran
     * once, n stayed 0, and the result came back as "delta 0, measured 0.000 g"
     * with the reference plainly sitting on the platform. */
    int64_t sum = 0;
    int n = 0;
    int64_t win = esp_timer_get_time() + (int64_t)CAL_AVERAGE_MS * 1000;
    while (esp_timer_get_time() < win) {
        vTaskDelay(pdMS_TO_TICKS(50));
        weight_task_get(&snap);
        sum += snap.counts;
        n++;
        cal_elapsed(t0);
    }

    double load = n ? (double)sum / n : (double)tare;
    double delta = load - tare;
    xSemaphoreTake(lock, pdMS_TO_TICKS(50));
    float old_cal = filter.cal_factor;
    xSemaphoreGive(lock);

    bool saved = false;
    double new_cal = 0.0;
    if (mass_hint > 0.0 && delta > 0.0) {
        new_cal = delta / mass_hint;
        weight_task_set_calibration((float)new_cal, tare);
        saved = true;
    }

    xSemaphoreTake(cal_lock, portMAX_DELAY);
    cal_state.delta = delta;
    cal_state.grams = delta / old_cal;
    cal_state.new_cal = new_cal;
    cal_state.saved = saved;
    xSemaphoreGive(cal_lock);
    {
        char msg[72];
        if (saved) {
            snprintf(msg, sizeof(msg), "cal_factor %.2f -> %.2f", (double)old_cal, new_cal);
        } else {
            snprintf(msg, sizeof(msg), "measured %.3f g at cal %.2f",
                     delta / old_cal, (double)old_cal);
        }
        cal_publish(CAL_DONE, msg);
    }

restore:
    xSemaphoreTake(lock, portMAX_DELAY);
    filter.azt_enabled = azt_was_on;
    xSemaphoreGive(lock);
    cal_running = false;
    vTaskDelete(NULL);
}

esp_err_t weight_task_cal_start(double mass_hint)
{
    if (cal_lock == NULL) {
        cal_lock = xSemaphoreCreateMutex();
        if (cal_lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (cal_running) {
        return ESP_ERR_INVALID_STATE;
    }
    cal_running = true;
    memset(&cal_state, 0, sizeof(cal_state));
    cal_mass_hint = mass_hint;
    /* Pin the wizard to core 1: it only polls the snapshot, and core 0 must stay
     * the sole owner of the HX711 read window. */
    BaseType_t ok = xTaskCreatePinnedToCore(cal_task, "cal", 4096, NULL, 4, NULL, 1);
    if (ok != pdPASS) {
        cal_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void weight_task_get(scale_snapshot_t *out)
{
    if (xSemaphoreTake(lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        *out = snapshot;
        return;
    }
    *out = snapshot;
    xSemaphoreGive(lock);
}

void weight_task_tare(void)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    scale_filter_tare(&filter, true);
    nvs_save(&filter);
    xSemaphoreGive(lock);
    ESP_LOGI(TAG, "tare taken");
}

void weight_task_set_unit(scale_unit_t unit)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    scale_filter_set_unit(&filter, unit);
    nvs_save(&filter);
    xSemaphoreGive(lock);
}

esp_err_t weight_task_set_calibration(float cal_factor, int32_t tare_offset)
{
    esp_err_t err;
    xSemaphoreTake(lock, portMAX_DELAY);
    scale_filter_set_calibration(&filter, cal_factor, tare_offset);
    err = nvs_save(&filter);
    xSemaphoreGive(lock);
    return err;
}

void weight_task_set_azt(bool enabled)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    filter.azt_enabled = enabled;
    xSemaphoreGive(lock);
}

float weight_task_get_ref_mass(void)
{
    return ref_mass;
}

esp_err_t weight_task_set_ref_mass(float mass)
{
    if (mass <= 0.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    ref_mass = mass;
    esp_err_t err = nvs_save(&filter);
    xSemaphoreGive(lock);
    return err;
}