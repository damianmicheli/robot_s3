#include "display.h"

#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"

/* Conexiones GMT020-02-7P / ST7789V */
#define TFT_MOSI_GPIO GPIO_NUM_11
#define TFT_SCLK_GPIO GPIO_NUM_12
#define TFT_CS_GPIO   GPIO_NUM_10
#define TFT_DC_GPIO   GPIO_NUM_13
#define TFT_RST_GPIO  GPIO_NUM_14
#define TFT_SPI_HOST  SPI2_HOST
#define TFT_WIDTH     240
#define TFT_HEIGHT    320

#define DISPLAY_FONT_NAME "merienda_24"
#define LVGL_BUFFER_LINES 40
#define TFT_SPI_CLOCK_HZ  (20 * 1000 * 1000)

/* Paleta de colores */
#define COLOR_BG            0x080B10
#define COLOR_CYAN          0x00E5FF
#define COLOR_RED           0xFF2A55
#define COLOR_AMBER         0xFFB300
#define COLOR_CHEEK         0xFF3366
#define COLOR_BADGE_BG      0x121722
#define COLOR_BADGE_BORDER  0x222B3D
#define COLOR_TEXT_MUTED    0x8898AA

typedef enum {
    FACE_STATE_STANDBY,
    FACE_STATE_RECORDING,
    FACE_STATE_THINKING,
    FACE_STATE_SPEAKING
} face_state_t;

typedef struct {
    int16_t w;
    int16_t h;
    int16_t r;
} mouth_frame_t;

/* Secuencia de visemas para animacion de habla natural */
static const mouth_frame_t s_speech_frames[] = {
    {42, 10, 5},
    {48, 22, 11},
    {36, 30, 15},
    {52, 16, 8},
    {44, 26, 13},
    {38, 14, 7},
    {54, 32, 16},
    {46, 18, 9},
};
#define SPEECH_FRAME_COUNT (sizeof(s_speech_frames) / sizeof(s_speech_frames[0]))

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;

/* Objetos graficos de la carita */
static lv_obj_t *s_eye_left;
static lv_obj_t *s_eye_right;
static lv_obj_t *s_pupil_left;
static lv_obj_t *s_pupil_right;
static lv_obj_t *s_cheek_left;
static lv_obj_t *s_cheek_right;
static lv_obj_t *s_mouth;
static lv_obj_t *s_status_badge;
static lv_obj_t *s_status_label;

static lv_timer_t *s_face_timer;
static face_state_t s_current_state = FACE_STATE_STANDBY;
static uint32_t s_state_ticks = 0;
static uint32_t s_anim_step = 0;

extern const lv_font_t font_merienda_24;

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

static const lv_font_t *font_from_name(const char *name)
{
    if (strcmp(name, "merienda_24") == 0) return &font_merienda_24;
    return &lv_font_montserrat_14;
}

/* Aplica el estado visual a la carita (debe llamarse con el mutex de LVGL tomado) */
static void apply_face_state(face_state_t state)
{
    s_current_state = state;
    s_state_ticks = 0;
    s_anim_step = 0;

    switch (state) {
        case FACE_STATE_STANDBY: {
            lv_color_t c = lv_color_hex(COLOR_CYAN);
            lv_obj_set_style_bg_color(s_eye_left, c, 0);
            lv_obj_set_style_bg_color(s_eye_right, c, 0);
            lv_obj_set_size(s_eye_left, 52, 72);
            lv_obj_set_size(s_eye_right, 52, 72);
            lv_obj_set_style_radius(s_eye_left, 24, 0);
            lv_obj_set_style_radius(s_eye_right, 24, 0);
            lv_obj_set_pos(s_eye_left, 46, 79);
            lv_obj_set_pos(s_eye_right, 142, 79);

            lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_pupil_left, 8, 8);
            lv_obj_set_pos(s_pupil_right, 8, 8);

            lv_obj_set_style_bg_color(s_mouth, c, 0);
            lv_obj_set_size(s_mouth, 46, 8);
            lv_obj_set_pos(s_mouth, 120 - 23, 205 - 4);
            lv_obj_set_style_radius(s_mouth, 4, 0);

            lv_label_set_text(s_status_label, "STAND BY");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
            break;
        }

        case FACE_STATE_RECORDING: {
            lv_color_t c = lv_color_hex(COLOR_RED);
            lv_obj_set_style_bg_color(s_eye_left, c, 0);
            lv_obj_set_style_bg_color(s_eye_right, c, 0);
            lv_obj_set_size(s_eye_left, 54, 76);
            lv_obj_set_size(s_eye_right, 54, 76);
            lv_obj_set_style_radius(s_eye_left, 26, 0);
            lv_obj_set_style_radius(s_eye_right, 26, 0);
            lv_obj_set_pos(s_eye_left, 45, 77);
            lv_obj_set_pos(s_eye_right, 141, 77);

            lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_pupil_left, 10, 10);
            lv_obj_set_pos(s_pupil_right, 10, 10);

            lv_obj_set_style_bg_color(s_mouth, c, 0);
            lv_obj_set_size(s_mouth, 24, 14);
            lv_obj_set_pos(s_mouth, 120 - 12, 205 - 7);
            lv_obj_set_style_radius(s_mouth, 7, 0);

            lv_label_set_text(s_status_label, "ESCUCHANDO");
            lv_obj_set_style_text_color(s_status_label, c, 0);
            break;
        }

        case FACE_STATE_THINKING: {
            lv_color_t c = lv_color_hex(COLOR_AMBER);
            lv_obj_set_style_bg_color(s_eye_left, c, 0);
            lv_obj_set_style_bg_color(s_eye_right, c, 0);
            lv_obj_set_size(s_eye_left, 50, 68);
            lv_obj_set_size(s_eye_right, 50, 68);
            lv_obj_set_style_radius(s_eye_left, 22, 0);
            lv_obj_set_style_radius(s_eye_right, 22, 0);
            lv_obj_set_pos(s_eye_left, 47, 74);
            lv_obj_set_pos(s_eye_right, 143, 74);

            lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(s_pupil_left, 10, 4);
            lv_obj_set_pos(s_pupil_right, 10, 4);

            lv_obj_set_style_bg_color(s_mouth, c, 0);
            lv_obj_set_size(s_mouth, 32, 6);
            lv_obj_set_pos(s_mouth, 120 - 16, 205 - 3);
            lv_obj_set_style_radius(s_mouth, 3, 0);

            lv_label_set_text(s_status_label, "PENSANDO");
            lv_obj_set_style_text_color(s_status_label, c, 0);
            break;
        }

        case FACE_STATE_SPEAKING: {
            lv_color_t c = lv_color_hex(COLOR_CYAN);
            lv_obj_set_style_bg_color(s_eye_left, c, 0);
            lv_obj_set_style_bg_color(s_eye_right, c, 0);
            lv_obj_set_size(s_eye_left, 52, 70);
            lv_obj_set_size(s_eye_right, 52, 70);
            lv_obj_set_style_radius(s_eye_left, 24, 0);
            lv_obj_set_style_radius(s_eye_right, 24, 0);
            lv_obj_set_pos(s_eye_left, 46, 80);
            lv_obj_set_pos(s_eye_right, 142, 80);

            lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);

            lv_obj_set_style_bg_color(s_mouth, c, 0);

            lv_label_set_text(s_status_label, "HABLANDO");
            lv_obj_set_style_text_color(s_status_label, c, 0);
            break;
        }
    }
}

/* Callback del temporizador LVGL (se ejecuta en la tarea de LVGL, con lock ya adquirido) */
static void face_timer_cb(lv_timer_t *timer)
{
    s_state_ticks++;

    if (s_current_state == FACE_STATE_SPEAKING) {
        /* Anima la boca sincronizada como si estuviera hablando */
        if (s_state_ticks % 2 == 0) {
            s_anim_step++;
            const mouth_frame_t f = s_speech_frames[s_anim_step % SPEECH_FRAME_COUNT];
            lv_obj_set_size(s_mouth, f.w, f.h);
            lv_obj_set_pos(s_mouth, 120 - (f.w / 2), 205 - (f.h / 2));
            lv_obj_set_style_radius(s_mouth, f.r, 0);

            /* Pequeno rebote en los ojos durante el habla para darle vida */
            int16_t eye_h = (s_anim_step & 1) ? 68 : 72;
            lv_obj_set_size(s_eye_left, 52, eye_h);
            lv_obj_set_size(s_eye_right, 52, eye_h);
        }
    } else if (s_current_state == FACE_STATE_STANDBY) {
        /* Parpadeo suave cada ~3.5 segundos (70 ticks de 50 ms) */
        uint32_t cycle = s_state_ticks % 70;
        if (cycle == 66 || cycle == 68) {
            /* Semi-cerrado */
            lv_obj_set_size(s_eye_left, 52, 24);
            lv_obj_set_size(s_eye_right, 52, 24);
            lv_obj_set_style_radius(s_eye_left, 12, 0);
            lv_obj_set_style_radius(s_eye_right, 12, 0);
            lv_obj_add_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
        } else if (cycle == 67) {
            /* Totalmente cerrado */
            lv_obj_set_size(s_eye_left, 52, 6);
            lv_obj_set_size(s_eye_right, 52, 6);
            lv_obj_set_style_radius(s_eye_left, 3, 0);
            lv_obj_set_style_radius(s_eye_right, 3, 0);
            lv_obj_add_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
        } else if (cycle == 69) {
            /* Vuelve a abrir */
            lv_obj_set_size(s_eye_left, 52, 72);
            lv_obj_set_size(s_eye_right, 52, 72);
            lv_obj_set_style_radius(s_eye_left, 24, 0);
            lv_obj_set_style_radius(s_eye_right, 24, 0);
            lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_current_state == FACE_STATE_THINKING) {
        /* Movimiento de pupilas mientras procesa */
        if (s_state_ticks % 8 == 0) {
            s_anim_step++;
            int16_t pupil_x = 6 + ((s_anim_step % 3) * 3);
            lv_obj_set_pos(s_pupil_left, pupil_x, 4);
            lv_obj_set_pos(s_pupil_right, pupil_x, 4);
        }
    }
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    /* Ojo izquierdo */
    s_eye_left = lv_obj_create(screen);
    lv_obj_remove_flag(s_eye_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eye_left, 52, 72);
    lv_obj_set_pos(s_eye_left, 46, 79);
    lv_obj_set_style_radius(s_eye_left, 24, 0);
    lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_eye_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_left, 0, 0);
    lv_obj_set_style_pad_all(s_eye_left, 0, 0);

    /* Pupila/brillo izquierdo */
    s_pupil_left = lv_obj_create(s_eye_left);
    lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pupil_left, 14, 18);
    lv_obj_set_pos(s_pupil_left, 8, 8);
    lv_obj_set_style_radius(s_pupil_left, 7, 0);
    lv_obj_set_style_bg_color(s_pupil_left, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_pupil_left, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_pupil_left, 0, 0);
    lv_obj_set_style_pad_all(s_pupil_left, 0, 0);

    /* Ojo derecho */
    s_eye_right = lv_obj_create(screen);
    lv_obj_remove_flag(s_eye_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eye_right, 52, 72);
    lv_obj_set_pos(s_eye_right, 142, 79);
    lv_obj_set_style_radius(s_eye_right, 24, 0);
    lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_eye_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eye_right, 0, 0);
    lv_obj_set_style_pad_all(s_eye_right, 0, 0);

    /* Pupila/brillo derecho */
    s_pupil_right = lv_obj_create(s_eye_right);
    lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pupil_right, 14, 18);
    lv_obj_set_pos(s_pupil_right, 8, 8);
    lv_obj_set_style_radius(s_pupil_right, 7, 0);
    lv_obj_set_style_bg_color(s_pupil_right, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_pupil_right, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_pupil_right, 0, 0);
    lv_obj_set_style_pad_all(s_pupil_right, 0, 0);

    /* Mejillas sonrosadas (rubor suave) */
    s_cheek_left = lv_obj_create(screen);
    lv_obj_remove_flag(s_cheek_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_cheek_left, 20, 8);
    lv_obj_set_pos(s_cheek_left, 44, 160);
    lv_obj_set_style_radius(s_cheek_left, 4, 0);
    lv_obj_set_style_bg_color(s_cheek_left, lv_color_hex(COLOR_CHEEK), 0);
    lv_obj_set_style_bg_opa(s_cheek_left, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_cheek_left, 0, 0);
    lv_obj_set_style_pad_all(s_cheek_left, 0, 0);

    s_cheek_right = lv_obj_create(screen);
    lv_obj_remove_flag(s_cheek_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_cheek_right, 20, 8);
    lv_obj_set_pos(s_cheek_right, 176, 160);
    lv_obj_set_style_radius(s_cheek_right, 4, 0);
    lv_obj_set_style_bg_color(s_cheek_right, lv_color_hex(COLOR_CHEEK), 0);
    lv_obj_set_style_bg_opa(s_cheek_right, LV_OPA_40, 0);
    lv_obj_set_style_border_width(s_cheek_right, 0, 0);
    lv_obj_set_style_pad_all(s_cheek_right, 0, 0);

    /* Boca animada */
    s_mouth = lv_obj_create(screen);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_mouth, 46, 8);
    lv_obj_set_pos(s_mouth, 120 - 23, 205 - 4);
    lv_obj_set_style_radius(s_mouth, 4, 0);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(COLOR_CYAN), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mouth, 0, 0);
    lv_obj_set_style_pad_all(s_mouth, 0, 0);

    /* Badge de estado inferior */
    s_status_badge = lv_obj_create(screen);
    lv_obj_remove_flag(s_status_badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_status_badge, 190, 38);
    lv_obj_align(s_status_badge, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(s_status_badge, 19, 0);
    lv_obj_set_style_bg_color(s_status_badge, lv_color_hex(COLOR_BADGE_BG), 0);
    lv_obj_set_style_bg_opa(s_status_badge, LV_OPA_90, 0);
    lv_obj_set_style_border_width(s_status_badge, 1, 0);
    lv_obj_set_style_border_color(s_status_badge, lv_color_hex(COLOR_BADGE_BORDER), 0);
    lv_obj_set_style_pad_all(s_status_badge, 0, 0);

    s_status_label = lv_label_create(s_status_badge);
    lv_obj_set_style_text_font(s_status_label, font_from_name(DISPLAY_FONT_NAME), 0);
    lv_obj_set_style_text_opa(s_status_label, LV_OPA_COVER, 0);
    lv_label_set_text(s_status_label, "STAND BY");
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(COLOR_TEXT_MUTED), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 0);

    /* Temporizador a 50 ms para animaciones fluidas */
    s_face_timer = lv_timer_create(face_timer_cb, 50, NULL);

    apply_face_state(FACE_STATE_STANDBY);
    lvgl_port_unlock();

    return ESP_OK;
}

void display_show_standby(void)
{
    lvgl_port_lock(0);
    apply_face_state(FACE_STATE_STANDBY);
    lvgl_port_unlock();
}

void display_show_recording(void)
{
    lvgl_port_lock(0);
    apply_face_state(FACE_STATE_RECORDING);
    lvgl_port_unlock();
}

void display_show_thinking(void)
{
    lvgl_port_lock(0);
    apply_face_state(FACE_STATE_THINKING);
    lvgl_port_unlock();
}

void display_show_speaking(void)
{
    lvgl_port_lock(0);
    apply_face_state(FACE_STATE_SPEAKING);
    lvgl_port_unlock();
}
