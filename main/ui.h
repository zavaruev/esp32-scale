/*
 * ui.h - LVGL interface on the 320x240 landscape TFT.
 *
 * Two screens: the normal weighing page and the calibration progress overlay.
 * The UI never reads the HX711 directly - it only consumes weight_task's
 * published snapshot, from a task pinned to core 1.
 */
#ifndef UI_H
#define UI_H

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

typedef enum {
    UI_PAGE_WEIGH = 0,
    UI_PAGE_CAL,
} ui_page_t;

/* The panel and IO handles come from st7789_init(); LVGL's port needs them to
 * attach its flush callback to the SPI display. */
esp_err_t ui_init(esp_lcd_panel_io_handle_t io, esp_lcd_panel_handle_t panel);
void ui_show_page(ui_page_t page);
void ui_toggle_hold(void);
void ui_clear_hold(void);
void ui_notify_activity(void);

#endif /* UI_H */
