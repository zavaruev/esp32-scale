/*
 * scale_app.c - application entry point and serial console.
 *
 * Ownership rule: weight_task is the ONLY reader of the HX711. Two tasks
 * clocking the same device would interleave pulse trains and corrupt both
 * readings, so every serial command here works through the published snapshot
 * instead of touching the bus directly.
 *
 * Serial console (115200):
 *   c          print the current reading
 *   t          tare
 *   R          dump the measurement task's own raw sample log
 *   x <0|1>    suspend/resume the WiFi radio
 *   d [min]    zero-stability monitor, one point per 30 s
 *   m [grams]  paired tare+load calibration; auto-detects the load
 *   w          print the filter constants and the shared state
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hx711.h"
#include "ota.h"
#include "scale_filter.h"
#include "web_server.h"
#include "weight_task.h"

/* Detection threshold for the paired calibration. A real reference is thousands
 * of counts above the tare; the worst measured drift is 6 counts/s. */
#define LOAD_DETECT_COUNTS 2000

static void print_reading(void)
{
    scale_snapshot_t s;
    weight_task_get(&s);
    printf("#RAW,%ld counts  mask=%lld us (max %lld)\n", (long)s.counts,
           (long long)hx711_mask_last_us, (long long)hx711_mask_max_us);
    printf("#WEIGHT,%.3f g  sigma %.0f mg  stable=%d zero=%d\n",
           (double)s.grams, (double)s.sigma_mg, s.stable, s.zero);
    printf("#CAL,cal=%.2f tare=%ld uptime=%lu s\n", (double)s.cal_factor,
           (long)s.tare_offset, (unsigned long)s.uptime_s);
    fflush(stdout);
}

static void tare_and_report(void)
{
    weight_task_tare();
    vTaskDelay(pdMS_TO_TICKS(1500));
    print_reading();
}

/*
 * Zero-stability monitor. The filter work is finished, so the remaining blocker
 * is zero wander of up to +/-0.5 g, which is a hardware property and cannot be
 * filtered away - only measured and then fixed at the source. If zero moves
 * steadily after a cold start it is thermal; if it jumps around it is supply.
 */
static void drift_monitor(int minutes)
{
    printf("#DRIFT_BEGIN,platform must stay EMPTY, %d minutes\n", minutes);
    fflush(stdout);

    scale_snapshot_t first = {0};
    for (int p = 0; p < minutes * 2; p++) {
        vTaskDelay(pdMS_TO_TICKS(20000));
        scale_snapshot_t s;
        weight_task_get(&s);
        if (p == 0) {
            first = s;
        }
        printf("#DRIFT,min=%.1f,counts=%ld,from_first=%ld,grams=%.3f,sigma=%.0f\n",
               p * 0.5, (long)s.counts, (long)(s.counts - first.counts),
               (double)(s.counts - first.counts) / s.cal_factor,
               (double)s.sigma_mg);
        fflush(stdout);
    }
    printf("#DRIFT_END\n");
    fflush(stdout);
}

/*
 * Paired calibration. Tare and load are captured back to back: the original
 * separated captures disagreed by up to 0.3% purely because zero drifted in the
 * gap between them, which put that error straight into cal_factor.
 */
static void paired_calibration(double mass_hint)
{
    printf("#BEGIN_PAIRED,platform must be EMPTY and left alone; waiting for it "
           "to settle\n");
    fflush(stdout);

    /* Wait for the platform to settle rather than a fixed 20 s.
     *
     * A fixed short window is a race: the tare gets captured while the platform
     * is still moving, and the error lands directly in cal_factor. Measured on
     * an empty platform, the reading settled by roughly 1.8 g within the first
     * 30 s after boot or after anyone touched it, so a 20 s window could
     * capture a tare that was 1.8 g wrong. The condition used here is the
     * filter's own STABLE flag - computed on raw samples, before the EMA - held
     * continuously for 5 s. */
    int64_t settle_start = esp_timer_get_time();
    int stable_ticks = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(250));
        scale_snapshot_t s;
        weight_task_get(&s);
        int64_t waited_ms = (esp_timer_get_time() - settle_start) / 1000;
        if (s.stable) {
            stable_ticks++;
        } else {
            stable_ticks = 0;
        }
        printf("#SETTLE,%lld ms,stable=%d,hold=%d\n", (long long)waited_ms,
               s.stable, stable_ticks);
        fflush(stdout);
        if (waited_ms > 240000) {
            printf("#SETTLE_TIMEOUT,using the reading as it stands\n");
            fflush(stdout);
            break;
        }
        if (waited_ms >= 15000 && stable_ticks >= 20) {
            break;
        }
    }

    scale_snapshot_t t;
    weight_task_get(&t);
    int32_t tare = t.counts;
    printf("#TARE,%ld counts,platform judged stable\n", (long)tare);
    printf("#PHASE2_WAITING,put the reference on the platform\n");
    fflush(stdout);

    /* Auto-detect the load so the operator is not part of the timing loop. */
    int detected = 0;
    int64_t deadline = esp_timer_get_time() + 300LL * 1000000;
    while (!detected && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(250));
        scale_snapshot_t s;
        weight_task_get(&s);
        long jump = (long)s.counts - tare;
        if (jump < 0) {
            jump = -jump;
        }
        if (jump > LOAD_DETECT_COUNTS) {
            detected = 1;
            printf("#DETECTED,jump=%ld counts = %.3f g, settling 3 s\n", jump,
                   (double)jump / s.cal_factor);
            fflush(stdout);
        }
        /* Drain operator keystrokes. */
        char c;
        read(STDIN_FILENO, &c, 1);
    }
    if (!detected) {
        printf("#ABORT,no load detected\n#END_PAIRED\n");
        fflush(stdout);
        return;
    }

    /* Settle, then average a window long enough to beat the noise. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    int64_t sum = 0;
    int n = 0;
    int64_t window_us = 20LL * 1000000;
    int64_t t0 = esp_timer_get_time();
    while (esp_timer_get_time() - t0 < window_us) {
        vTaskDelay(pdMS_TO_TICKS(50));
        scale_snapshot_t s;
        weight_task_get(&s);
        sum += s.counts;
        n++;
    }
    double load = n ? (double)sum / n : 0.0;
    long delta = (long)llround(load - tare);
    double grams = delta / t.counts ? delta / t.cal_factor : 0.0;

    printf("#LOAD,%.0f counts,delta=%ld = %.3f g\n", load, delta, grams);
    if (mass_hint > 0.0) {
        double cf = delta / mass_hint;
        printf("#CAL_FACTOR,%.2f (stored %.2f, %+.3f%%)\n", cf,
               (double)t.cal_factor, 100.0 * (cf - t.cal_factor) / t.cal_factor);
        weight_task_set_calibration((float)cf, tare);
        printf("#SAVED,cal_factor now %.2f, tare %ld\n", cf, (long)tare);
    } else {
        printf("#CAL_FACTOR,pass a mass as 'm <grams>' to apply\n");
    }
    printf("#END_PAIRED\n");
    fflush(stdout);
}

/*
 * Raw stream. Prints one line per FRESH HX711 sample, so the capture rate is the
 * true 11.3 Hz rather than the polling rate. Two questions need this:
 *   - how often the raw signal jumps far outside its own noise, which no
 *     post-filter can repair, only count;
 *   - whether the noise floor changes with WiFi transmitting, which points at
 *     the 3.3 V rail feeding the HX711 rather than at the mechanics.
 */
static void raw_dump(void)
{
    static raw_log_entry_t buf[RAW_LOG_N];   /* 16 KB, .bss */

    weight_task_raw_log_clear();
    printf("#RAW_CLEARED,waiting 45 s while the measurement task records by itself\n");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(45000));

    int n = weight_task_raw_log(buf, RAW_LOG_N);
    printf("#RAW_BEGIN,%d samples captured by the task itself (no polling)\n", n);
    printf("#RAW_META,read_timeouts=%u,last_wait_ms=%.1f\n",
           (unsigned)weight_task_read_failures(),
           (double)weight_task_last_wait_us() / 1000.0);
    for (int i = 0; i < n; i++) {
        printf("#S,%lu,%lld,%ld\n", (unsigned long)buf[i].seq,
               (long long)buf[i].t_us, (long)buf[i].counts);
    }
    printf("#RAW_END,%d samples\n", n);
    fflush(stdout);
}

static void print_constants(void)
{
    scale_snapshot_t s;
    weight_task_get(&s);
    printf("#STATE,cal_factor=%.2f,tare_offset=%ld,unit=%d,sigma=%.0f mg\n",
           (double)s.cal_factor, (long)s.tare_offset, (int)s.unit,
           (double)s.sigma_mg);
    fflush(stdout);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500));

    ESP_ERROR_CHECK(weight_task_start());
    /* Before the server comes up, so the page's first status reply already
     * knows which slot is running. ota_init() also confirms the running image
     * if the bootloader started it as PENDING_VERIFY, which is what lets an
     * update stick instead of reverting on the next restart. */
    ota_init();
    ESP_ERROR_CHECK(net_start());
    wifi_start_ap_fallback();

    printf("#READY commands: c | t | d [min] | m [grams] | w | R(raw log) | x 0|1 (wifi) | s(scan) | h(selftest) | a 0..2 | p 0..1 | k <password> | b <MAC> | T <dBm>\n");
    fflush(stdout);

    char line[64];
    int li = 0;

    for (;;) {
        char ch;
        ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got == 1 && (ch == '\n' || ch == '\r')) {
            if (li > 0) {
                line[li] = '\0';
                /* The argument is copied out before line[] is truncated at the
                 * space, otherwise "k <password>" would pass an empty string. */
                char *arg = strchr(line, ' ');
                char argbuf[64] = {0};
                if (arg) {
                    strncpy(argbuf, arg + 1, sizeof(argbuf) - 1);
                    *arg = '\0';
                }
                double num = atof(argbuf);
                switch (line[0]) {
                case 'c': print_reading(); break;
                case 't': tare_and_report(); break;
                case 'w': print_constants(); break;
                case 's': wifi_scan_report(); break;
                case 'h': web_selftest(); break;
                case 'G':
                    hx711_set_irq_guard((int)num != 0);
                    printf("#IRQ_GUARD,%d\n", (int)num);
                    break;
                case 'T':
                    printf("#TX_POWER,%d\n", wifi_set_tx_power((int)num));
                    break;
                case 'L':
                    wifi_set_debug_log((int)num != 0);
                    break;
                case 'M':
                    wifi_set_mac(argbuf);
                    break;
                case 'b':
                    wifi_set_bssid(argbuf);
                    fflush(stdout);
                    break;
                case 'k':
                    wifi_set_password(argbuf);
                    fflush(stdout);
                    break;
                case 'p':
                    wifi_set_pmf((int)num != 0);
                    printf("#PMF,%d\n", (int)num);
                    fflush(stdout);
                    break;
                case 'a':
                    wifi_set_auth_mode((int)num);
                    printf("#AUTH_MODE,%d\n", (int)num);
                    fflush(stdout);
                    break;
                case 'R': raw_dump(); break;
                case 'x':
                    wifi_suspend(num > 0);   /* x 1 = radio off, x 0 = radio on */
                    fflush(stdout);
                    break;
                case 'd': drift_monitor(num > 0 ? (int)num : 20); break;
                case 'm': paired_calibration(num); break;
                default: break;
                }
                li = 0;
                fflush(stdout);
            }
        } else if (got == 1 && li < (int)sizeof(line) - 1) {
            line[li++] = ch;
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}