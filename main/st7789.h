/*
 * st7789.h - 2.0" GMT020-02-8P TFT (ST7789V, 240x320) on SPI2.
 *
 * The module is mounted turned 90 degrees in the front panel, so after
 * swap_xy the logical framebuffer is 320x240 (landscape).
 *
 * Wiring, by ESP32-S3 GPIO:
 *   SCLK 12   MOSI 11   CS 10   DC 2   RST 4   BL 5
 *
 * SDA/SCL on this module's silkscreen are SPI lines, NOT I2C.
 * The module is 3.3 V only.
 */
#ifndef ST7789_H
#define ST7789_H

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"

#define ST7789_PIN_SCLK GPIO_NUM_12
#define ST7789_PIN_MOSI GPIO_NUM_11
#define ST7789_PIN_CS   GPIO_NUM_10
#define ST7789_PIN_DC   GPIO_NUM_2
#define ST7789_PIN_RST  GPIO_NUM_4
#define ST7789_PIN_BL   GPIO_NUM_5

/* Native panel size. The panel is kept in PORTRAIT (240x320) on purpose - the
 * driver from the module's own reference code drives exactly this window with
 * MADCTL=0x00. The 90 degree mount rotation is done in LVGL (sw_rotate), not
 * here: hardware swap_xy on this controller leaves the left third of the panel
 * unwritten and turns the picture sideways. */
#define ST7789_NATIVE_W 240
#define ST7789_NATIVE_H 320
#define ST7789_HRES     240
#define ST7789_VRES     320

/* MADCTL=0x00 for this module (no swap, no mirror, RGB order). */
#define ST7789_SWAP_XY  false
#define ST7789_MIRROR_X false
#define ST7789_MIRROR_Y false
/* The module's reference code sends 0x21 = display inversion ON. */
#define ST7789_INVERT   true

/* The module's reference code uses 20 MHz. Keep that value: 27 MHz over long
 * jumper wires is less reliable. */
#define ST7789_PIXEL_CLOCK_HZ (20000000)   /* reference value, long-wire safe */

esp_err_t st7789_init(esp_lcd_panel_io_handle_t *out_io,
                      esp_lcd_panel_handle_t *out_panel);
void st7789_backlight(bool on);

#endif /* ST7789_H */
