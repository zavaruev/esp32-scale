/*
 * ssd1306.h - 0.96" OLED 128x64 over I2C (SSD1306), portrait.
 *
 * Wiring: SDA = GPIO14, SCL = GPIO21, VCC = 3V3 (or 5V - most modules have a
 * regulator, but 3.3 V keeps the logic levels safe), GND = GND.
 *
 * A tiny hand-written framebuffer renderer: no LVGL, no draw task, no mutex.
 * The framebuffer is 128*64/8 = 1024 bytes. Redraw happens in one task only.
 */
#ifndef SSD1306_H
#define SSD1306_H

#include "esp_err.h"

#define OLED_PIN_SDA GPIO_NUM_14
#define OLED_PIN_SCL GPIO_NUM_21
#define OLED_WIDTH   128
#define OLED_HEIGHT  64

/* Initialise I2C + SSD1306 panel, clear to black. */
esp_err_t ssd1306_init(void);

/* Framebuffer helpers. Coordinates: x 0..127, y 0..63, origin top-left. */
void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
/* 5x7 font text. Returns the x position after the last glyph. */
int  oled_text(int x, int y, const char *str, bool on);
/* Large 16x32 digits + '.' and '-', for the weight readout. */
int  oled_big_text(int x, int y, const char *str, bool on);
/* Push the framebuffer to the panel. */
esp_err_t oled_flush(void);

#endif /* SSD1306_H */
