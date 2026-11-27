/*
 * st7789.c - initialise the SPI bus, the panel IO and the ST7789 controller.
 *
 * Everything here runs once from app_main, i.e. on the core that calls it. The
 * subsequent pixel traffic is driven by LVGL's task, which is pinned to core 1
 * (see ui.c). Core 0 remains the sole owner of the HX711.
 */
#include <string.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "st7789.h"

static const char *TAG = "st7789";

esp_err_t st7789_init(esp_lcd_panel_io_handle_t *out_io,
                      esp_lcd_panel_handle_t *out_panel)
{
    esp_err_t err;

    /* MISO is not used: the ST7789 is write-only in this application. Leave the
     * pin at -1 so it stays available for something else. */
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = ST7789_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = ST7789_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        /* One partial LVGL buffer is 320*40 pixels = 25.6 kB. */
        .max_transfer_sz = ST7789_HRES * 40 * 2,
    };
    err = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = ST7789_PIN_DC,
        .cs_gpio_num = ST7789_PIN_CS,
        .pclk_hz = ST7789_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    err = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = ST7789_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    esp_lcd_panel_handle_t panel = NULL;
    err = esp_lcd_new_panel_st7789(io, &panel_cfg, &panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789: %s", esp_err_to_name(err));
        return err;
    }

    /* ---- Full reset sequence from the module's working reference code ----
     * The stock esp_lcd reset/init is minimal (10 ms hardware pulse, no NORON).
     * This panel occasionally comes up in a bad state and shows black; drive it
     * through the complete, slow init sequence instead. */

    /* 1. Hardware reset: long LOW then HIGH with generous settling. */
    gpio_set_level(ST7789_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(5));
    gpio_set_level(ST7789_PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(ST7789_PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(150));

    /* 2. Software reset + sleep out, with the datasheet's slow timings. */
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 3. MADCTL / COLMOD / RAMCTRL / invert / gap. No hardware reset here so
     *    we do not undo the sequence above. */
    err = esp_lcd_panel_init(panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init: %s", esp_err_to_name(err));
        return err;
    }
    esp_lcd_panel_invert_color(panel, ST7789_INVERT);
    esp_lcd_panel_set_gap(panel, 0, 0);

    /* 4. Normal display mode ON, then display ON (what the reference sends). */
    esp_lcd_panel_io_tx_param(io, LCD_CMD_NORON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    esp_lcd_panel_io_tx_param(io, LCD_CMD_DISPON, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(20));

    /* Orientation is applied by LVGL's port via rotation.swap_xy / mirror. */

    /* Backlight is a plain GPIO on/off - no PWM, so no switching current is
     * coupled into the 3.3 V rail that also feeds the HX711. */
    gpio_config_t bl = {
        .pin_bit_mask = 1ULL << ST7789_PIN_BL,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&bl);
    st7789_backlight(true);

    *out_io = io;
    *out_panel = panel;

    ESP_LOGI(TAG, "ST7789 ready, %dx%d @ %d MHz",
             ST7789_HRES, ST7789_VRES, ST7789_PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

void st7789_backlight(bool on)
{
    gpio_set_level(ST7789_PIN_BL, on ? 1 : 0);
}
