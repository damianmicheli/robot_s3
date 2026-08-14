#include "display.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_random.h"
#include "esp_rom_sys.h"

/*
 * GMT020-02-7P (ST7789V), interfaz SPI de 4 hilos.
 * Ajusta estos cinco GPIO a los que uses en tu ESP32-S3.
 * SDA de la pantalla es MOSI y SCL es SCLK (no son I2C).
 */
#define TFT_PIN_MOSI GPIO_NUM_11
#define TFT_PIN_SCLK GPIO_NUM_12
#define TFT_PIN_CS   GPIO_NUM_10
#define TFT_PIN_DC   GPIO_NUM_13
#define TFT_PIN_RST  GPIO_NUM_14

#define TFT_HOST     SPI2_HOST
#define TFT_WIDTH    240
#define TFT_HEIGHT   320
#define TFT_CMD_MODE 0
#define TFT_DATA_MODE 1

static spi_device_handle_t s_tft;

static void tft_write(bool data, const void *buffer, size_t length)
{
    gpio_set_level(TFT_PIN_DC, data ? TFT_DATA_MODE : TFT_CMD_MODE);
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = buffer,
    };
    ESP_ERROR_CHECK(spi_device_polling_transmit(s_tft, &transaction));
}

static void tft_command(uint8_t command, const uint8_t *data, size_t length)
{
    tft_write(false, &command, 1);
    if (length) {
        tft_write(true, data, length);
    }
}

static void tft_window(uint16_t x, uint16_t y, uint16_t width, uint16_t height)
{
    uint8_t area[] = { x >> 8, x, (uint16_t)(x + width - 1) >> 8,
                       (uint8_t)(x + width - 1) };
    tft_command(0x2A, area, sizeof(area));
    uint8_t page[] = { y >> 8, y, (uint16_t)(y + height - 1) >> 8,
                       (uint8_t)(y + height - 1) };
    tft_command(0x2B, page, sizeof(page));
    tft_command(0x2C, NULL, 0);
}

static void tft_fill(uint16_t color)
{
    uint8_t pixels[240 * 2];
    for (size_t i = 0; i < sizeof(pixels); i += 2) {
        pixels[i] = color >> 8;
        pixels[i + 1] = color;
    }
    tft_window(0, 0, TFT_WIDTH, TFT_HEIGHT);
    for (int line = 0; line < TFT_HEIGHT; line++) {
        tft_write(true, pixels, sizeof(pixels));
    }
}

/* Fuente 5x7 para las letras usadas por el mensaje. */
static const uint8_t font[][5] = {
    [' '] = {0x00, 0x00, 0x00, 0x00, 0x00},
    ['a'] = {0x20, 0x54, 0x54, 0x54, 0x78},
    ['b'] = {0x7F, 0x48, 0x44, 0x44, 0x38},
    ['d'] = {0x38, 0x44, 0x44, 0x48, 0x7F},
    ['e'] = {0x38, 0x54, 0x54, 0x54, 0x18},
    ['i'] = {0x00, 0x44, 0x7D, 0x40, 0x00},
    ['n'] = {0x7C, 0x08, 0x04, 0x04, 0x78},
    ['o'] = {0x38, 0x44, 0x44, 0x44, 0x38},
    ['p'] = {0x7C, 0x14, 0x14, 0x14, 0x08},
    ['r'] = {0x7C, 0x08, 0x04, 0x04, 0x08},
    ['s'] = {0x48, 0x54, 0x54, 0x54, 0x24},
    ['t'] = {0x04, 0x3F, 0x44, 0x40, 0x20},
};

static void tft_pixel(uint16_t x, uint16_t y, uint16_t color)
{
    uint8_t pixel[] = { color >> 8, color };
    tft_window(x, y, 1, 1);
    tft_write(true, pixel, sizeof(pixel));
}

static void tft_char(uint16_t x, uint16_t y, char c, uint8_t scale, uint16_t color)
{
    for (int column = 0; column < 5; column++) {
        uint8_t bits = font[(unsigned char)c][column];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int dx = 0; dx < scale; dx++) {
                    for (int dy = 0; dy < scale; dy++) {
                        tft_pixel(x + column * scale + dx, y + row * scale + dy, color);
                    }
                }
            }
        }
    }
}

esp_err_t display_init(void)
{
    gpio_config_t reset_config = {
        .pin_bit_mask = 1ULL << TFT_PIN_DC | 1ULL << TFT_PIN_RST,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&reset_config));

    spi_bus_config_t bus_config = {
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = TFT_PIN_SCLK,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
    };
    spi_device_interface_config_t device_config = {
        .clock_speed_hz = 20 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = TFT_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_HOST, &bus_config, SPI_DMA_CH_AUTO), "TFT", "SPI init");
    ESP_RETURN_ON_ERROR(spi_bus_add_device(TFT_HOST, &device_config, &s_tft), "TFT", "SPI device");

    gpio_set_level(TFT_PIN_RST, 0);
    esp_rom_delay_us(20 * 1000);
    gpio_set_level(TFT_PIN_RST, 1);
    esp_rom_delay_us(120 * 1000);

    tft_command(0x01, NULL, 0);                 // Software reset
    esp_rom_delay_us(150 * 1000);
    tft_command(0x11, NULL, 0);                 // Sleep out
    esp_rom_delay_us(120 * 1000);
    const uint8_t color_mode = 0x55;            // RGB565
    const uint8_t orientation = 0x00;           // Orientación nativa: 240 x 320
    const uint8_t porch[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    const uint8_t gate = 0x35;
    const uint8_t vcom = 0x28;
    const uint8_t lcm = 0x2C;
    const uint8_t vdv_vrhs_enable = 0x01;
    const uint8_t vrhs = 0x0B;
    const uint8_t vdv = 0x20;
    const uint8_t frame_rate = 0x0F;
    const uint8_t power[] = { 0xA4, 0xA1 };
    const uint8_t gamma_positive[] = {
        0xD0, 0x01, 0x08, 0x0F, 0x11, 0x2A, 0x36,
        0x55, 0x44, 0x3A, 0x0B, 0x06, 0x11, 0x20,
    };
    const uint8_t gamma_negative[] = {
        0xD0, 0x02, 0x07, 0x0A, 0x0B, 0x18, 0x34,
        0x43, 0x4A, 0x2B, 0x1B, 0x1C, 0x22, 0x1F,
    };

    /* Parámetros recomendados por ST7789V; eliminan artefactos en el panel. */
    tft_command(0xB2, porch, sizeof(porch));
    tft_command(0xB7, &gate, 1);
    tft_command(0xBB, &vcom, 1);
    tft_command(0xC0, &lcm, 1);
    tft_command(0xC2, &vdv_vrhs_enable, 1);
    tft_command(0xC3, &vrhs, 1);
    tft_command(0xC4, &vdv, 1);
    tft_command(0xC6, &frame_rate, 1);
    tft_command(0xD0, power, sizeof(power));
    tft_command(0xE0, gamma_positive, sizeof(gamma_positive));
    tft_command(0xE1, gamma_negative, sizeof(gamma_negative));
    tft_command(0x3A, &color_mode, 1);
    tft_command(0x36, &orientation, 1);
    tft_command(0x21, NULL, 0);                 // Inversión necesaria en esta pantalla
    tft_command(0x29, NULL, 0);                 // Display on
    tft_fill(0x0000);
    return ESP_OK;
}

void display_show_button_pressed(void)
{
    static const char message[] = "boton presionado";
    const uint8_t scale = 2;
    const uint16_t text_width = (sizeof(message) - 1) * 6 * scale;
    const uint16_t x = (TFT_WIDTH - text_width) / 2;
    const uint16_t y = (TFT_HEIGHT - 7 * scale) / 2;
    const uint16_t background = (uint16_t)esp_random();
    const uint8_t red = background >> 11;
    const uint8_t green = (background >> 5) & 0x3F;
    const uint8_t blue = background & 0x1F;
    const uint16_t text_color = (red * 299 + green * 150 + blue * 299 > 12000)
                                    ? 0x0000
                                    : 0xFFFF;

    tft_fill(background);
    for (size_t i = 0; i < sizeof(message) - 1; i++) {
        tft_char(x + i * 6 * scale, y, message[i], scale, text_color);
    }
}
