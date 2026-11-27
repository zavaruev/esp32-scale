/* Pure decision helper for the periodic idle-zero autosave.
 * No ESP-IDF headers here on purpose: tools/test_idle_tare.c compiles this
 * logic on the host. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    int64_t last_us;     /* 0 until the first call; reset on every decision */
    int64_t period_us;
} idle_tare_t;

/* Returns true when the stored tare should be refreshed from the current
 * resting point. The timer starts on the first call (no write at boot) and
 * resets on every call after the period, whether or not a save happened. */
bool idle_tare_due(idle_tare_t *s, int64_t now_us, bool settled, bool zero);
