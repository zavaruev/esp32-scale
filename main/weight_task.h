/*
 * weight_task.h - measurement task pinned to core 0, plus shared state.
 *
 * Per the hardware spec: core 0 owns acquisition and filtering, core 1 owns the
 * user interface. They communicate only through this locked snapshot, never by
 * sharing the filter object directly.
 */
#ifndef WEIGHT_TASK_H
#define WEIGHT_TASK_H

#include "esp_err.h"
#include "scale_filter.h"

typedef struct {
    float   display_value;   /* in the selected unit */
    float   grams;
    float   sigma_mg;        /* raw noise, for the diagnostics panel */
    bool    stable;
    bool    zero;
    bool    tare_active;
    bool    azt_enabled;
    int32_t counts;
    float   cal_factor;
    int32_t tare_offset;
    scale_unit_t unit;
    /* AUTO resolved against the averaged reading, shared by UI and web. */
    scale_unit_t effective_unit;
    uint32_t uptime_s;
    /* Increments once per fresh HX711 read. A reader polling faster than the
     * 11.3 Hz conversion rate would otherwise be unable to tell a new sample
     * from a repeat of the previous one, which is what makes the raw glitch
     * rate measurable at all. */
    uint32_t sample_seq;
    bool    updated;

    /* Mean of the raw counts over a window measured in SAMPLES, not in polls.
     *
     * The browser used to do this averaging itself, across polls 1.5 s apart.
     * That is the same arithmetic at a hundredth of the data rate: 12 polls is
     * 12 numbers, while the device produces 11.3 of them every second and
     * throws 17 away between polls. Averaging here costs no wall-clock time at
     * all, so the same precision arrives in a quarter of the time.
     *
     * Expressed relative to the same auto-zero anchor as grams, so tare, deadband
     * and the zero flag all keep meaning exactly what they did. */
    float   avg_grams;
    /* Standard error of that mean, from the measured raw sigma and the window
     * length. This is what makes the number honest: it says how much the figure
     * can still be trusted, rather than leaving the page to guess from polls. */
    float   avg_sigma_mg;
    uint16_t avg_n;
} scale_snapshot_t;

/* Loads calibration from NVS, starts the measurement task on core 0. */
esp_err_t weight_task_start(void);

void weight_task_get(scale_snapshot_t *out);

void weight_task_tare(void);
void weight_task_set_unit(scale_unit_t unit);
/* Diagnostics: a ring buffer filled by the measurement task itself.
 *
 * Reading the samples by polling the snapshot is not good enough. The console
 * task runs at priority 6 on the same core as the measurement task, so a poll
 * loop keeps both cores' run queues permanently non-empty, IDLE0 never runs and
 * the task watchdog panics - the instrument changes what it measures. Having the
 * producer record into a buffer lets the console dump it afterwards with no
 * polling at all. */
/* 1024 entries is ~90 s at 11.3 Hz, which covers a whole warm-up or drift
 * window. A larger ring plus a copy-out buffer overflowed dram0_0_seg by 62 KB. */
#define RAW_LOG_N 1024

typedef struct {
    uint32_t seq;
    int64_t  t_us;
    int32_t  counts;
} raw_log_entry_t;

/* Copies up to n entries into out, oldest first. Returns the number written. */
int weight_task_raw_log(raw_log_entry_t *out, int n);
/* Number of entries currently held. */
int weight_task_raw_log_count(void);
void weight_task_raw_log_clear(void);

/* Consecutive HX711 read timeouts since the last success. */
uint32_t weight_task_read_failures(void);
/* Microseconds the most recent successful read spent waiting for DOUT. */
int64_t weight_task_last_wait_us(void);

/* ------------------------------------------------------------- calibration */

/* Phases of the two-point calibration. Published so the web UI can show progress
 * instead of guessing; the work runs in its own task and the HTTP handlers only
 * read this state, so the server is never blocked for the ~30 s it takes. */
typedef enum {
    CAL_IDLE = 0,
    CAL_SETTLING,      /* waiting for the empty platform to stop moving */
    CAL_WAIT_LOAD,     /* tare taken, waiting for the reference to appear */
    CAL_MEASURING,     /* averaging the loaded platform */
    CAL_DONE,
    CAL_ABORT,
} cal_phase_t;

typedef struct {
    cal_phase_t phase;
    int32_t tare;          /* counts, valid once phase leaves CAL_SETTLING */
    double   delta;        /* counts, valid once phase is CAL_DONE */
    double   grams;        /* mass implied by the stored cal_factor */
    double   new_cal;      /* cal_factor implied by the reference, mass > 0 */
    bool     saved;        /* new_cal written to NVS */
    int64_t  elapsed_ms;
    char     detail[72];
} cal_state_t;

/* Starts the paired calibration. mass_hint > 0 applies and stores the resulting
 * cal_factor; mass_hint <= 0 measures only and reports both numbers without
 * touching NVS. Returns ESP_ERR_INVALID_STATE if one is already running. */
esp_err_t weight_task_cal_start(double mass_hint);
void weight_task_cal_get(cal_state_t *out);

/* Writes calibration to NVS and applies it live. */
esp_err_t weight_task_set_calibration(float cal_factor, int32_t tare_offset);
void weight_task_set_azt(bool enabled);

/* Reference mass (grams) used by the one-touch calibration on the front panel.
 * Persisted in NVS; the default is 73.03 g, the weight used for the paired
 * measurements in AGENTS.md. Set it once from the web page or the `F` command. */
float weight_task_get_ref_mass(void);
esp_err_t weight_task_set_ref_mass(float mass);

#endif /* WEIGHT_TASK_H */