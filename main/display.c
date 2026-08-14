#include "display.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"

/* GMT020-02-7P: SDA es MOSI y SCL es SCLK (no I2C). */
#define TFT_PIN_MOSI GPIO_NUM_11
#define TFT_PIN_SCLK GPIO_NUM_12
#define TFT_PIN_CS   GPIO_NUM_10
#define TFT_PIN_DC   GPIO_NUM_13
#define TFT_PIN_RST  GPIO_NUM_14
#define TFT_HOST     SPI2_HOST
#define TFT_WIDTH    240
#define TFT_HEIGHT   320

static esp_lcd_panel_handle_t s_panel;
static esp_lcd_panel_io_handle_t s_panel_io;
static lv_obj_t *s_message_label;

/* Fuente Roboto 24 px generada desde el archivo TTF con lv_font_conv. */
extern const lv_font_t font_roboto_24;
extern const lv_font_t font_merienda_18;
extern const lv_font_t font_merienda_24;

/* Escribe los parámetros eléctricos específicos que eliminan el ruido del panel. */
static void configure_gmt020_panel(void)
{
    static const uint8_t porch[] = { 0x0C, 0x0C, 0x00, 0x33, 0x33 };
    static const uint8_t power[] = { 0xA4, 0xA1 };
    static const uint8_t gamma_pos[] = { 0xD0, 0x01, 0x08, 0x0F, 0x11, 0x2A, 0x36, 0x55, 0x44, 0x3A, 0x0B, 0x06, 0x11, 0x20 };
    static const uint8_t gamma_neg[] = { 0xD0, 0x02, 0x07, 0x0A, 0x0B, 0x18, 0x34, 0x43, 0x4A, 0x2B, 0x1B, 0x1C, 0x22, 0x1F };
    static const uint8_t one[] = { 0x35, 0x28, 0x2C, 0x01, 0x0B, 0x20, 0x0F };
    static const struct { uint8_t cmd; const uint8_t *data; size_t len; } commands[] = {
        {0xB2, porch, sizeof(porch)}, {0xB7, &one[0], 1}, {0xBB, &one[1], 1},
        {0xC0, &one[2], 1}, {0xC2, &one[3], 1}, {0xC3, &one[4], 1},
        {0xC4, &one[5], 1}, {0xC6, &one[6], 1}, {0xD0, power, sizeof(power)},
        {0xE0, gamma_pos, sizeof(gamma_pos)}, {0xE1, gamma_neg, sizeof(gamma_neg)},
    };
    for (size_t i = 0; i < sizeof(commands) / sizeof(commands[0]); i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io, commands[i].cmd, commands[i].data, commands[i].len));
    }
}

/* Permite elegir la fuente desde una cadena, sin exponer detalles de LVGL al resto del programa. */
static const lv_font_t *font_from_name(const char *name)
{
    if (strcmp(name, "roboto_24") == 0) return &font_roboto_24;
    if (strcmp(name, "merienda_18") == 0) return &font_merienda_18;
    if (strcmp(name, "merienda_24") == 0) return &font_merienda_24;
    if (strcmp(name, "montserrat_14") == 0) return &lv_font_montserrat_14;
    return &lv_font_montserrat_14; /* Fuente segura por defecto. */
}

esp_err_t display_init(void)
{
    const spi_bus_config_t bus = {
        .mosi_io_num = TFT_PIN_MOSI, .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = TFT_PIN_SCLK, .quadwp_io_num = GPIO_NUM_NC, .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TFT_WIDTH * 40 * sizeof(uint16_t),
    };
    const esp_lcd_panel_io_spi_config_t io = {
        .cs_gpio_num = TFT_PIN_CS, .dc_gpio_num = TFT_PIN_DC, .spi_mode = 0,
        .pclk_hz = 20 * 1000 * 1000, .trans_queue_depth = 10, .lcd_cmd_bits = 8, .lcd_param_bits = 8,
    };
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = TFT_PIN_RST, .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE, .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_HOST, &bus, SPI_DMA_CH_AUTO), "TFT", "SPI");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(TFT_HOST, &io, &s_panel_io), "TFT", "IO SPI");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_cfg, &s_panel), "TFT", "ST7789V");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), "TFT", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), "TFT", "init");
    configure_gmt020_panel();
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), "LVGL", "init");
    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_panel_io, .panel_handle = s_panel,
        .buffer_size = TFT_WIDTH * 40, .double_buffer = true,
        .hres = TFT_WIDTH, .vres = TFT_HEIGHT,
        .flags = {.buff_dma = true},
    };
    if (lvgl_port_add_disp(&display_cfg) == NULL) return ESP_ERR_NO_MEM;

    lvgl_port_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    s_message_label = lv_label_create(screen);
    lv_label_set_text(s_message_label, "");
    lv_obj_set_style_text_opa(s_message_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(s_message_label, font_from_name("merienda_24"), 0);
    lv_obj_set_style_bg_opa(s_message_label, LV_OPA_TRANSP, 0);
    lv_obj_center(s_message_label);
    lvgl_port_unlock();
    return ESP_OK;
}

void display_show_button_pressed(void)
{
    const uint32_t color = esp_random() & 0x00FFFFFF;
    const uint8_t red = color >> 16;
    const uint8_t green = color >> 8;
    const uint8_t blue = color;
    const bool light_background = red * 299 + green * 587 + blue * 114 > 128000;

    /* Texto negro en fondos claros y blanco en fondos oscuros. */
    lvgl_port_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(color), 0);
    lv_obj_set_style_text_color(s_message_label,
                                light_background ? lv_color_black() : lv_color_white(), 0);
    lv_label_set_text(s_message_label, "boton presionado");
    lv_obj_remove_flag(s_message_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_message_label, LV_ALIGN_CENTER, 0, 0);
    lv_obj_invalidate(s_message_label);
    lvgl_port_unlock();
}
