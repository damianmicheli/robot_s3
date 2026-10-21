#include "display.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"

/* Conexiones: SDA es MOSI y SCL es SCLK; esta pantalla no usa I2C. */
#define TFT_MOSI_GPIO GPIO_NUM_11
#define TFT_SCLK_GPIO GPIO_NUM_12
#define TFT_CS_GPIO   GPIO_NUM_10
#define TFT_DC_GPIO   GPIO_NUM_13
#define TFT_RST_GPIO  GPIO_NUM_14
#define TFT_SPI_HOST  SPI2_HOST
#define TFT_WIDTH     240
#define TFT_HEIGHT    320

#define DISPLAY_FONT_NAME       "merienda_24"
#define LVGL_BUFFER_LINES       40
#define TFT_SPI_CLOCK_HZ        (20 * 1000 * 1000)

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;
static lv_obj_t *s_message_label;

/* Fuente personalizada, generada desde Merienda-Variable.ttf con lv_font_conv. */
extern const lv_font_t font_merienda_24;

/*
 * El driver ST7789V de ESP-IDF inicializa lo básico. Esta pantalla requiere
 * además estos valores de voltaje, gamma y temporización para no mostrar ruido.
 */
static void configure_gmt020_panel(void)
{
    static const uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    static const uint8_t power[] = {0xA4, 0xA1};
    static const uint8_t controls[] = {0x35, 0x28, 0x2C, 0x01, 0x0B, 0x20, 0x0F};
    static const uint8_t gamma_positive[] = {0xD0, 0x01, 0x08, 0x0F, 0x11, 0x2A, 0x36, 0x55, 0x44, 0x3A, 0x0B, 0x06, 0x11, 0x20};
    static const uint8_t gamma_negative[] = {0xD0, 0x02, 0x07, 0x0A, 0x0B, 0x18, 0x34, 0x43, 0x4A, 0x2B, 0x1B, 0x1C, 0x22, 0x1F};
    static const struct {
        uint8_t command;
        const uint8_t *data;
        size_t length;
    } settings[] = {
        {0xB2, porch, sizeof(porch)}, {0xB7, &controls[0], 1},
        {0xBB, &controls[1], 1}, {0xC0, &controls[2], 1},
        {0xC2, &controls[3], 1}, {0xC3, &controls[4], 1},
        {0xC4, &controls[5], 1}, {0xC6, &controls[6], 1},
        {0xD0, power, sizeof(power)}, {0xE0, gamma_positive, sizeof(gamma_positive)},
        {0xE1, gamma_negative, sizeof(gamma_negative)},
    };

    for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io,
                        settings[i].command, settings[i].data, settings[i].length));
    }
}

/* Cambia este selector para alternar entre la fuente personalizada y la integrada. */
static const lv_font_t *font_from_name(const char *name)
{
    if (strcmp(name, "merienda_24") == 0) return &font_merienda_24;
    return &lv_font_montserrat_14;
}

esp_err_t display_init(void)
{
    const spi_bus_config_t bus_config = {
        .mosi_io_num = TFT_MOSI_GPIO,
        .miso_io_num = GPIO_NUM_NC,
        .sclk_io_num = TFT_SCLK_GPIO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = TFT_WIDTH * LVGL_BUFFER_LINES * sizeof(uint16_t),
    };
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = TFT_CS_GPIO,
        .dc_gpio_num = TFT_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = TFT_SPI_CLOCK_HZ,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = TFT_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };

    ESP_RETURN_ON_ERROR(spi_bus_initialize(TFT_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO), "TFT", "SPI");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(TFT_SPI_HOST, &io_config, &s_panel_io), "TFT", "IO SPI");
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_panel_io, &panel_config, &s_panel), "TFT", "ST7789V");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), "TFT", "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), "TFT", "init");
    configure_gmt020_panel();
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

    ESP_RETURN_ON_ERROR(lvgl_port_init(&(lvgl_port_cfg_t)ESP_LVGL_PORT_INIT_CONFIG()), "LVGL", "init");
    const lvgl_port_display_cfg_t lvgl_display = {
        .io_handle = s_panel_io,
        .panel_handle = s_panel,
        .buffer_size = TFT_WIDTH * LVGL_BUFFER_LINES,
        .double_buffer = true,
        .hres = TFT_WIDTH,
        .vres = TFT_HEIGHT,
        .flags = {.buff_dma = true},
    };
    if (lvgl_port_add_disp(&lvgl_display) == NULL) return ESP_ERR_NO_MEM;

    lvgl_port_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    s_message_label = lv_label_create(screen);
    lv_obj_set_style_text_font(s_message_label, font_from_name(DISPLAY_FONT_NAME), 0);
    lv_obj_set_style_text_opa(s_message_label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(s_message_label, LV_OPA_TRANSP, 0);
    lvgl_port_unlock();
    return ESP_OK;
}

static void display_show_status(const char *message, uint32_t background, lv_color_t text_color)
{
    /* LVGL necesita su mutex porque atiende la interfaz en su propia tarea. */
    lvgl_port_lock(0);
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(background), 0);
    lv_obj_set_style_text_color(s_message_label, text_color, 0);
    lv_label_set_text(s_message_label, message);
    lv_obj_align(s_message_label, LV_ALIGN_CENTER, 0, 0);
    lvgl_port_unlock();
}

void display_show_standby(void)
{
    display_show_status("STAND BY", 0x000000, lv_color_white());
}

void display_show_recording(void)
{
    display_show_status("GRABANDO", 0xFF0000, lv_color_white());
}

void display_show_speaking(void)
{
    display_show_status("HABLANDO", 0x0040C0, lv_color_white());
}
