#include "idle_tare.h"

bool idle_tare_due(idle_tare_t *s, int64_t now_us, bool settled, bool zero)
{
    if (s->last_us == 0) {
        s->last_us = now_us;          /* no NVS write on boot */
        return false;
    }
    if (now_us - s->last_us < s->period_us) {
        return false;
    }
    s->last_us = now_us;              /* reset whether or not we save */
    return settled && zero;
}
