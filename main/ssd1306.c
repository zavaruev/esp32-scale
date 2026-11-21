/*
 * ssd1306.c - 0.96" I2C OLED 128x64, logical PORTRAIT 64x128.
 *
 * The logical framebuffer is 64 wide x 128 tall. In oled_flush() it is rotated
 * 90 degrees into the panel's native 128x64 GDDRAM (page = 8 rows). A real
 * rotation (not a transpose/mirror) keeps glyphs the right way up.
 */
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"

#include "ssd1306.h"

static const char *TAG = "ssd1306";

#define OLED_I2C_ADDR  0x3C
#define OLED_I2C_CLK   400000
#define PHYS_W 128
#define PHYS_H 64

static esp_lcd_panel_handle_t s_panel;
/* Logical portrait FB: bit (x,y) at s_fb[x + (y>>3)*64]. */
static uint8_t s_fb[OLED_WIDTH * OLED_HEIGHT / 8];
/* Native FB after rotation: 128 columns x 8 pages. */
static uint8_t s_native[PHYS_W * PHYS_H / 8];

/* 5x7 font: [0]=char, [1..5]=columns, bit0=top. */
static const uint8_t font5x7[][6] = {
    {' ',0,0,0,0,0},{'!',0x00,0x00,0x5F,0x00,0x00},{'"',0x00,0x07,0x00,0x07,0x00},
    {'#',0x14,0x7F,0x14,0x7F,0x14},{'$',0x24,0x2A,0x7F,0x2A,0x12},{'%',0x23,0x13,0x08,0x64,0x62},
    {'&',0x36,0x49,0x55,0x22,0x50},{'\'',0x00,0x05,0x03,0x00,0x00},{'(',0x00,0x1C,0x22,0x41,0x00},
    {')',0x00,0x41,0x22,0x1C,0x00},{'*',0x14,0x08,0x3E,0x08,0x14},{'+',0x08,0x08,0x3E,0x08,0x08},
    {',',0x00,0x50,0x30,0x00,0x00},{'-',0x08,0x08,0x08,0x08,0x08},{'.',0x00,0x60,0x60,0x00,0x00},
    {'/',0x20,0x10,0x08,0x04,0x02},{'0',0x3E,0x51,0x49,0x45,0x3E},{'1',0x00,0x42,0x7F,0x40,0x00},
    {'2',0x42,0x61,0x51,0x49,0x46},{'3',0x21,0x41,0x45,0x4B,0x31},{'4',0x18,0x14,0x12,0x7F,0x10},
    {'5',0x27,0x45,0x45,0x45,0x39},{'6',0x3C,0x4A,0x49,0x49,0x30},{'7',0x01,0x71,0x09,0x05,0x03},
    {'8',0x36,0x49,0x49,0x49,0x36},{'9',0x06,0x49,0x49,0x29,0x1E},{':',0x00,0x36,0x36,0x00,0x00},
    {';',0x00,0x56,0x36,0x00,0x00},{'<',0x08,0x14,0x22,0x41,0x00},{'=',0x14,0x14,0x14,0x14,0x14},
    {'>',0x00,0x41,0x22,0x14,0x08},{'?',0x02,0x01,0x51,0x09,0x06},{'@',0x32,0x49,0x79,0x41,0x3E},
    {'A',0x7E,0x11,0x11,0x11,0x7E},{'B',0x7F,0x49,0x49,0x49,0x36},{'C',0x3E,0x41,0x41,0x41,0x22},
    {'D',0x7F,0x41,0x41,0x22,0x1C},{'E',0x7F,0x49,0x49,0x49,0x41},{'F',0x7F,0x09,0x09,0x01,0x01},
    {'G',0x3E,0x41,0x49,0x49,0x7A},{'H',0x7F,0x08,0x08,0x08,0x7F},{'I',0x00,0x41,0x7F,0x41,0x00},
    {'J',0x20,0x40,0x41,0x3F,0x01},{'K',0x7F,0x08,0x14,0x22,0x41},{'L',0x7F,0x40,0x40,0x40,0x40},
    {'M',0x7F,0x02,0x0C,0x02,0x7F},{'N',0x7F,0x04,0x08,0x10,0x7F},{'O',0x3E,0x41,0x41,0x41,0x3E},
    {'P',0x7F,0x09,0x09,0x09,0x06},{'Q',0x3E,0x41,0x51,0x21,0x5E},{'R',0x7F,0x09,0x19,0x29,0x46},
    {'S',0x46,0x49,0x49,0x49,0x31},{'T',0x01,0x01,0x7F,0x01,0x01},{'U',0x3F,0x40,0x40,0x40,0x3F},
    {'V',0x1F,0x20,0x40,0x20,0x1F},{'W',0x7F,0x20,0x18,0x20,0x7F},{'X',0x63,0x14,0x08,0x14,0x63},
    {'Y',0x03,0x04,0x78,0x04,0x03},{'Z',0x61,0x51,0x49,0x45,0x43},{'[',0x00,0x7F,0x41,0x41,0x00},
    {'\\',0x02,0x04,0x08,0x10,0x20},{']',0x00,0x41,0x41,0x7F,0x00},{'^',0x04,0x02,0x01,0x02,0x04},
    {'_',0x40,0x40,0x40,0x40,0x40},
    /* Lowercase letters used by the unit labels (g, kg, oz). Without these,
     * glyph() returned NULL and the units were silently never drawn. */
    {'g',0x00,0x3D,0x42,0x42,0x3D},{'k',0x40,0x40,0x5C,0x62,0x42},
    {'o',0x00,0x3C,0x42,0x42,0x3C},{'z',0x00,0x62,0x52,0x4A,0x46},
};

static const uint8_t *glyph(uint8_t c)
{
    for (unsigned i = 0; i < sizeof(font5x7)/sizeof(font5x7[0]); i++) {
        if (font5x7[i][0] == c) return font5x7[i] + 1;
    }
    return NULL;
}

void oled_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }

void oled_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) return;
    uint8_t *b = &s_fb[x + (y >> 3) * OLED_WIDTH];
    if (on) *b |=  (1 << (y & 7));
    else    *b &= ~(1 << (y & 7));
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            oled_pixel(i, j, on);
}

int oled_text(int x, int y, const char *str, bool on)
{
    int cx = x;
    for (const char *p = str; *p; p++) {
        const uint8_t *g = glyph((uint8_t)*p);
        if (!g) { cx += 6; continue; }
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1 << row)) oled_pixel(cx + col, y + row, on);
        cx += 6;
    }
    return cx;
}

/* Крупные цифры 5x7 * 3 = 15x21, шаг 18. Точка и минус - отдельно. */
int oled_big_text(int x, int y, const char *str, bool on)
{
    int cx = x;
    for (const char *p = str; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c == '.') {
            oled_fill_rect(cx + 6, y + 18, 3, 3, on);
            cx += 12;
            continue;
        }
        if (c == '-') {
            oled_fill_rect(cx + 3, y + 9, 9, 3, on);
            cx += 15;
            continue;
        }
        const uint8_t *g = glyph(c);   /* digits and letters, scaled alike */
        if (!g) { cx += 18; continue; }
        for (int col = 0; col < 5; col++)
            for (int row = 0; row < 7; row++)
                if (g[col] & (1 << row))
                    oled_fill_rect(cx + col*3, y + row*3, 3, 3, on);
        cx += 18;
    }
    return cx;
}

/* Настоящий поворот на 90 градусов (не отражение!).
 * Логическая точка (x,y) -> физическая (col,row):
 *   CW :  col = 127 - y, row = x
 *   CCW:  col = y,       row = 63 - x
 */
esp_err_t oled_flush(void)
{
    memset(s_native, 0, sizeof(s_native));
    for (int y = 0; y < OLED_HEIGHT; y++) {
        for (int x = 0; x < OLED_WIDTH; x++) {
            if (!(s_fb[x + (y >> 3) * OLED_WIDTH] & (1 << (y & 7)))) continue;
#if OLED_ROTATE_CW
            int col = PHYS_W - 1 - y;   /* 0..127 */
            int row = x;                /* 0..63  */
#else
            int col = y;                /* 0..127 */
            int row = PHYS_H - 1 - x;   /* 0..63  */
#endif
            if (col < 0 || col >= PHYS_W || row < 0 || row >= PHYS_H) continue;
            s_native[col + (row >> 3) * PHYS_W] |= (1 << (row & 7));
        }
    }
    return esp_lcd_panel_draw_bitmap(s_panel, 0, 0, PHYS_W, PHYS_H, s_native);
}

esp_err_t ssd1306_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = I2C_NUM_0,
        .sda_io_num = OLED_PIN_SDA,
        .scl_io_num = OLED_PIN_SCL,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &bus);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c bus: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr = OLED_I2C_ADDR,
        .scl_speed_hz = OLED_I2C_CLK,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .dc_bit_offset = 6,
    };
    err = esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel io: %s", esp_err_to_name(err)); return err; }

    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = PHYS_H };
    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd_cfg,
    };
    err = esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_panel);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel: %s", esp_err_to_name(err)); return err; }

    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_init(s_panel);
    if (err == ESP_OK) err = esp_lcd_panel_disp_on_off(s_panel, true);
    if (err != ESP_OK) { ESP_LOGE(TAG, "panel init: %s", esp_err_to_name(err)); return err; }

    oled_clear();
    oled_flush();
    ESP_LOGI(TAG, "SSD1306 ready, PORTRAIT %dx%d (rotated to 128x64)",
             OLED_WIDTH, OLED_HEIGHT);
    return ESP_OK;
}
