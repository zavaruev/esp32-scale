/* Host-side test for the idle-zero autosave decision.
 * Run in the esp-idf-scale container:
 *   gcc -I main tools/test_idle_tare.c main/idle_tare.c -o /tmp/test_idle_tare
 *   /tmp/test_idle_tare
 */
#include <assert.h>
#include <stdio.h>

#include "idle_tare.h"

int main(void)
{
    idle_tare_t s = { .last_us = 0, .period_us = 1000 };

    /* First call only starts the clock: no write at boot. */
    assert(!idle_tare_due(&s, 100, true, true));
    assert(s.last_us == 100);

    /* Before the period: no write even when everything is calm. */
    assert(!idle_tare_due(&s, 500, true, true));

    /* Load on the platform (zero == false): no write, but the timer resets. */
    assert(!idle_tare_due(&s, 1100, true, false));
    assert(s.last_us == 1100);
    assert(!idle_tare_due(&s, 1200, true, true));

    /* Period elapsed with an empty, settled platform: write. */
    assert(idle_tare_due(&s, 2100, true, true));

    /* Not again immediately after a write. */
    assert(!idle_tare_due(&s, 2200, true, true));

    puts("idle_tare: all assertions passed");
    return 0;
}
