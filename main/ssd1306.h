/*
 * ssd1306.h - 0.96" OLED 128x64 over I2C, used in PORTRAIT (logical 64x128).
 *
 * Wiring: SDA = GPIO14, SCL = GPIO21, VCC = 3V3, GND = GND.
 * A real 90 degree rotation is applied in oled_flush(), so text stays upright.
 */
#ifndef SSD1306_H
#define SSD1306_H

#include "esp_err.h"
#include "driver/gpio.h"

#define OLED_PIN_SDA GPIO_NUM_14
#define OLED_PIN_SCL GPIO_NUM_21

/* Logical portrait size. */
#define OLED_WIDTH  64
#define OLED_HEIGHT 128

/* Rotation direction: 0 = counterclockwise, 1 = clockwise.
 * If the letters lie tilted when flipped — set it to 1. */
#define OLED_ROTATE_CW 1   /* the orientation the user confirmed as correct */

esp_err_t ssd1306_init(void);

void oled_clear(void);
void oled_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
int  oled_text(int x, int y, const char *str, bool on);
/* Bold variant of oled_text - strokes +1 px, pitch 7: labels look a bit bigger. */
int  oled_text_bold(int x, int y, const char *str);
/* Large digits: 5x7, scale 3x = 15x21 px, pitch 18. */
int  oled_big_text(int x, int y, const char *str, bool on);
/* То же, но с масштабом s (1-3): для длинных строк (миллиграммы). */
int  oled_big_text_s(int x, int y, const char *str, int s);
/* 7-segment digits 11x21, pitch 13 — compact and always fit. */
int  oled_7seg(int x, int y, const char *str, bool on);
esp_err_t oled_flush(void);

#endif /* SSD1306_H */
