/*
 * buttons.h - 6-key membrane matrix on the front panel.
 *
 * The ribbon is a 2x3 matrix: two row wires (contacts 1 and 5) and three
 * column wires (contacts 2, 3, 4). All six intersections are populated:
 *
 *              col2(8)  col3(9)  col4(13)
 *   row1(6)     ED       SUM       0
 *   row5(7)     T        ITT       SBR
 *
 * (Clarified 2026-11-21: the lower three keys are 2-5, 3-5 and 4-5, so the
 * common wire of that group is contact 5, not contact 6. GPIO15 is freed.)
 *
 * Rows are driven LOW one at a time; the inactive row is left floating (INPUT),
 * never driven HIGH - otherwise two keys in one column would short the rows.
 * Columns are inputs with internal pull-ups, active LOW. No GPIO interrupts:
 * the whole thing is polled by a task pinned to core 1.
 */
#ifndef BUTTONS_H
#define BUTTONS_H

#include "esp_err.h"

typedef enum {
    BTN_ED = 0,   /* units */
    BTN_SUM,      /* modes / calibration */
    BTN_ZERO,     /* zero */
    BTN_T,        /* tare / AZT */
    BTN_ITT,      /* hold result */
    BTN_SBR,      /* reset */
    BTN_COUNT,
} button_id_t;

typedef enum {
    BTN_SHORT = 0,
    BTN_LONG,
} button_press_t;

typedef void (*button_handler_t)(button_id_t id, button_press_t press);

/* Starts the scanning task on core 1. The handler is called from that task, so
 * it must not block or touch the HX711. */
esp_err_t buttons_init(button_handler_t handler);

/* Debug helper: prints the raw matrix to the serial console once. */
void buttons_dump(void);

/* Returns a bitmask of currently pressed keys (bit = button_id_t).
 * Used by the /api/buttons diagnostic endpoint so the matrix can be tested
 * without a USB console. */
uint8_t buttons_state(void);

#endif /* BUTTONS_H */
