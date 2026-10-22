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

/*
 * Buffer ampliado y reloj SPI a 40 MHz con doble buffer DMA para
 * eliminar el screen-tearing y refrescar dentro del intervalo vertical.
 */
#define LVGL_BUFFER_LINES 80
#define TFT_SPI_CLOCK_HZ  (40 * 1000 * 1000)

/*
 * Paleta exacta de Bombon / Blossom (The Powerpuff Girls):
 * #FAD5C8 piel
 * #F8959F iris y lengua
 * #020301 pupila, delineado, boca y linea de pestana
 * #FEFEFE blanco del ojo y reflejo unico
 * #C1B9D8 parpado cerrado al pestanear
 */
#define COLOR_SKIN          0xFAD5C8
#define COLOR_IRIS          0xF8959F
#define COLOR_PUPIL         0x020301
#define COLOR_WHITE         0xFEFEFE
#define COLOR_EYELID        0xC1B9D8

/* Dimensiones faciales estilo clasico Las Chicas Superpoderosas */
#define FACE_CENTER_X       120
#define EYE_CENTER_Y        138
#define EYE_LEFT_X          64
#define EYE_RIGHT_X         176

/* Ojos circulares, pupila grande, iris angosto y reflejo unico */
#define EYE_SIZE            92
#define IRIS_SIZE           74
#define PUPIL_SIZE          60
#define SHINE_SIZE          18
#define EYELID_LINE_W       74
#define EYELID_LINE_H       4
#define MOUTH_CENTER_Y      232

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

/* Secuencia de visemas para animacion de habla */
static const mouth_frame_t s_speech_frames[] = {
    {20, 8, 4},
    {26, 14, 7},
    {22, 10, 5},
    {28, 16, 8},
    {24, 12, 6},
    {20, 8, 4},
    {30, 16, 8},
    {24, 12, 6},
};
#define SPEECH_FRAME_COUNT (sizeof(s_speech_frames) / sizeof(s_speech_frames[0]))

static esp_lcd_panel_io_handle_t s_panel_io;
static esp_lcd_panel_handle_t s_panel;

/* Objetos graficos de Bombon */
static lv_obj_t *s_eyes_row;
static lv_obj_t *s_eye_left;
static lv_obj_t *s_eye_right;
static lv_obj_t *s_iris_left;
static lv_obj_t *s_iris_right;
static lv_obj_t *s_pupil_left;
static lv_obj_t *s_pupil_right;
static lv_obj_t *s_shine_left;
static lv_obj_t *s_shine_right;
static lv_obj_t *s_eyelid_line_left;
static lv_obj_t *s_eyelid_line_right;
static lv_obj_t *s_mouth;
static lv_obj_t *s_tongue;
static lv_obj_t *s_smile_arc;

static lv_timer_t *s_face_timer;
static face_state_t s_current_state = FACE_STATE_STANDBY;
static uint32_t s_state_ticks = 0;
static uint32_t s_anim_step = 0;

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

/*
 * Sonrisa clasica de Las Chicas Superpoderosas:
 * Trazo curvo en U anti-aliased (#020301) con extremos redondeados,
 * alegre, tierna y sin la apariencia de asombro de una boca abierta.
 */
static void show_smile_arc(int16_t diameter, int16_t offset_x, int16_t offset_y, int16_t start_angle, int16_t end_angle)
{
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_smile_arc, LV_OBJ_FLAG_HIDDEN);

    lv_arc_set_bg_angles(s_smile_arc, start_angle, end_angle);
    lv_obj_set_size(s_smile_arc, diameter, diameter);
    lv_obj_set_pos(s_smile_arc,
                   FACE_CENTER_X + offset_x - (diameter / 2),
                   MOUTH_CENTER_Y + offset_y - (diameter / 2));
    lv_obj_invalidate(s_smile_arc);
}

/* Direcciona la mirada de ambos ojos simultaneamente */
static void set_eyes_gaze(int16_t iris_x, int16_t iris_y)
{
    lv_obj_set_pos(s_iris_left, iris_x, iris_y);
    lv_obj_set_pos(s_iris_right, iris_x, iris_y);
    lv_obj_invalidate(s_eyes_row);
}

/* Aplica el estado visual facial de Bombon */
static void apply_face_state(face_state_t state)
{
    s_current_state = state;
    s_state_ticks = 0;
    s_anim_step = 0;

    /* Asegura la visibilidad de los ojos abiertos en blanco con pupila grande e iris angosto */
    lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_remove_flag(s_iris_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_iris_right, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);

    switch (state) {
        case FACE_STATE_STANDBY: {
            set_eyes_gaze((EYE_SIZE - IRIS_SIZE) / 2, (EYE_SIZE - IRIS_SIZE) / 2);
            lv_obj_set_pos(s_pupil_left, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
            lv_obj_set_pos(s_pupil_right, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);

            /* Sonrisa en U alegre y dulce (tipo Chicas Superpoderosas) */
            show_smile_arc(24, 0, 0, 20, 160);
            break;
        }

        case FACE_STATE_RECORDING: {
            /* Ojos centrados y atentos */
            set_eyes_gaze((EYE_SIZE - IRIS_SIZE) / 2, (EYE_SIZE - IRIS_SIZE) / 2);
            lv_obj_set_pos(s_pupil_left, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
            lv_obj_set_pos(s_pupil_right, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);

            /* Sonrisa atenta y dulce escuchando al usuario */
            show_smile_arc(22, 0, 0, 25, 155);
            break;
        }

        case FACE_STATE_THINKING: {
            /* Mirada pensativa simultanea hacia arriba y a la izquierda */
            set_eyes_gaze(4, 4);
            lv_obj_set_pos(s_pupil_left, 5, 5);
            lv_obj_set_pos(s_pupil_right, 5, 5);

            /* Sonrisa curva sutil en U mientras piensa */
            show_smile_arc(20, 2, 0, 30, 150);
            break;
        }

        case FACE_STATE_SPEAKING: {
            set_eyes_gaze((EYE_SIZE - IRIS_SIZE) / 2, (EYE_SIZE - IRIS_SIZE) / 2);
            lv_obj_set_pos(s_pupil_left, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
            lv_obj_set_pos(s_pupil_right, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
            lv_obj_add_flag(s_smile_arc, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_tongue, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }

    lv_obj_invalidate(s_eyes_row);
}

/* Callback del temporizador LVGL (50 ms) */
static void face_timer_cb(lv_timer_t *timer)
{
    s_state_ticks++;

    if (s_current_state == FACE_STATE_SPEAKING) {
        /* Animacion fluida de boca hablando */
        if (s_state_ticks % 2 == 0) {
            s_anim_step++;
            const mouth_frame_t f = s_speech_frames[s_anim_step % SPEECH_FRAME_COUNT];
            lv_obj_set_size(s_mouth, f.w, f.h);
            lv_obj_set_pos(s_mouth, FACE_CENTER_X - (f.w / 2), MOUTH_CENTER_Y - (f.h / 2));
            lv_obj_set_style_radius(s_mouth, f.r, 0);

            int16_t tw = (f.w * 3) / 4;
            int16_t th = (f.h * 2) / 3;
            if (th < 4) th = 4;
            if (tw < 8) tw = 8;
            lv_obj_set_size(s_tongue, tw, th);
            lv_obj_set_pos(s_tongue, (f.w - tw) / 2, f.h - th + 1);
            lv_obj_set_style_radius(s_tongue, th / 2, 0);
        }
    } else if (s_current_state == FACE_STATE_STANDBY) {
        /*
         * Animacion de reposo con vida propia (ciclo de 200 ticks = 10 segundos):
         * - Mira al frente con Sonrisa en U dulce y alegre.
         * - Pestanea con parpado lavanda #C1B9D8 y linea central #020301.
         * - Mira a la derecha con Sonrisa picara en U (estilo Bellota).
         * - Mira abajo a la izquierda con Sonrisa tierna en U (estilo Burbuja).
         * - Pestanea de nuevo.
         * - Mira a la izquierda con Sonrisa amplia en U.
         * - Regresa al centro sonriendo.
         */
        uint32_t step = s_state_ticks % 200;

        if (step == 0) {
            /* Frente: Mirada al centro, sonrisa en U alegre */
            set_eyes_gaze(9, 9);
            show_smile_arc(24, 0, 0, 20, 160);
        } else if (step == 48 || step == 49) {
            /* Primer pestaneo sincronizado (100 ms) */
            lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_EYELID), 0);
            lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_EYELID), 0);
            lv_obj_add_flag(s_iris_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_iris_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_eyes_row);
        } else if (step == 50) {
            /* Reabre ojos mirando hacia la derecha con Sonrisa picara en U */
            lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_WHITE), 0);
            lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_WHITE), 0);
            lv_obj_remove_flag(s_iris_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_iris_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);
            set_eyes_gaze(14, 9);
            show_smile_arc(22, 4, 0, 15, 155);
        } else if (step == 95) {
            /* Mirada abajo a la izquierda con Sonrisa tierna en U */
            set_eyes_gaze(4, 14);
            show_smile_arc(20, -3, 2, 30, 150);
        } else if (step == 140 || step == 141) {
            /* Segundo pestaneo sincronizado (100 ms) */
            lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_EYELID), 0);
            lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_EYELID), 0);
            lv_obj_add_flag(s_iris_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_iris_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_eyes_row);
        } else if (step == 142) {
            /* Reabre ojos mirando a la izquierda con Sonrisa amplia en U */
            lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_WHITE), 0);
            lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_WHITE), 0);
            lv_obj_remove_flag(s_iris_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_iris_right, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);
            set_eyes_gaze(4, 9);
            show_smile_arc(26, -2, 0, 25, 165);
        } else if (step == 185) {
            /* Regresa al centro con Sonrisa centrada en U */
            set_eyes_gaze(9, 9);
            show_smile_arc(24, 0, 0, 20, 160);
        }
    } else if (s_current_state == FACE_STATE_THINKING) {
        if (s_state_ticks % 8 == 0) {
            s_anim_step++;
            int16_t iris_x = 3 + ((s_anim_step % 3) * 2);
            set_eyes_gaze(iris_x, 4);
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
    /* Fondo con el tono de piel exacto de Bombon (#FAD5C8) */
    lv_obj_set_style_bg_color(screen, lv_color_hex(COLOR_SKIN), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_set_style_border_width(screen, 0, 0);

    /*
     * Contenedor horizontal que engloba ambos ojos en una sola fila.
     * Al invalidar esta area, LVGL dibuja ambos ojos simultaneamente por franjas horizontales.
     */
    s_eyes_row = lv_obj_create(screen);
    lv_obj_remove_flag(s_eyes_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eyes_row, TFT_WIDTH, EYE_SIZE + 4);
    lv_obj_set_pos(s_eyes_row, 0, EYE_CENTER_Y - (EYE_SIZE / 2));
    lv_obj_set_style_bg_opa(s_eyes_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_eyes_row, 0, 0);
    lv_obj_set_style_pad_all(s_eyes_row, 0, 0);

    /* Ojo izquierdo redondo (Blanco #FEFEFE con delineado negro #020301 de 3px) */
    s_eye_left = lv_obj_create(s_eyes_row);
    lv_obj_remove_flag(s_eye_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eye_left, EYE_SIZE, EYE_SIZE);
    lv_obj_set_pos(s_eye_left, EYE_LEFT_X - (EYE_SIZE / 2), 0);
    lv_obj_set_style_radius(s_eye_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_left, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_bg_opa(s_eye_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_eye_left, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_border_width(s_eye_left, 3, 0);
    lv_obj_set_style_clip_corner(s_eye_left, true, 0);
    lv_obj_set_style_pad_all(s_eye_left, 0, 0);

    /* Linea horizontal negra en el centro para el parpado cerrado (#020301) */
    s_eyelid_line_left = lv_obj_create(s_eye_left);
    lv_obj_remove_flag(s_eyelid_line_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eyelid_line_left, EYELID_LINE_W, EYELID_LINE_H);
    lv_obj_set_pos(s_eyelid_line_left, (EYE_SIZE - EYELID_LINE_W) / 2, (EYE_SIZE - EYELID_LINE_H) / 2);
    lv_obj_set_style_radius(s_eyelid_line_left, 2, 0);
    lv_obj_set_style_bg_color(s_eyelid_line_left, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_bg_opa(s_eyelid_line_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eyelid_line_left, 0, 0);
    lv_obj_set_style_pad_all(s_eyelid_line_left, 0, 0);
    lv_obj_add_flag(s_eyelid_line_left, LV_OBJ_FLAG_HIDDEN);

    /* Iris izquierdo mas angosto (Rosa Bombon #F8959F con borde negro 2px) */
    s_iris_left = lv_obj_create(s_eye_left);
    lv_obj_remove_flag(s_iris_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_iris_left, IRIS_SIZE, IRIS_SIZE);
    lv_obj_set_pos(s_iris_left, (EYE_SIZE - IRIS_SIZE) / 2, (EYE_SIZE - IRIS_SIZE) / 2);
    lv_obj_set_style_radius(s_iris_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_iris_left, lv_color_hex(COLOR_IRIS), 0);
    lv_obj_set_style_bg_opa(s_iris_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_iris_left, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_border_width(s_iris_left, 2, 0);
    lv_obj_set_style_clip_corner(s_iris_left, true, 0);
    lv_obj_set_style_pad_all(s_iris_left, 0, 0);

    /* Pupila izquierda bastante grande (#020301) */
    s_pupil_left = lv_obj_create(s_iris_left);
    lv_obj_remove_flag(s_pupil_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pupil_left, PUPIL_SIZE, PUPIL_SIZE);
    lv_obj_set_pos(s_pupil_left, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
    lv_obj_set_style_radius(s_pupil_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pupil_left, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_bg_opa(s_pupil_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pupil_left, 0, 0);
    lv_obj_set_style_clip_corner(s_pupil_left, true, 0);
    lv_obj_set_style_pad_all(s_pupil_left, 0, 0);

    /* Unico reflejo blanco (#FEFEFE) en la pupila */
    s_shine_left = lv_obj_create(s_pupil_left);
    lv_obj_remove_flag(s_shine_left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_shine_left, SHINE_SIZE, SHINE_SIZE);
    lv_obj_set_pos(s_shine_left, 14, 10);
    lv_obj_set_style_radius(s_shine_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shine_left, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_bg_opa(s_shine_left, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_shine_left, 0, 0);
    lv_obj_set_style_pad_all(s_shine_left, 0, 0);

    /* Ojo derecho redondo */
    s_eye_right = lv_obj_create(s_eyes_row);
    lv_obj_remove_flag(s_eye_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eye_right, EYE_SIZE, EYE_SIZE);
    lv_obj_set_pos(s_eye_right, EYE_RIGHT_X - (EYE_SIZE / 2), 0);
    lv_obj_set_style_radius(s_eye_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_eye_right, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_bg_opa(s_eye_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_eye_right, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_border_width(s_eye_right, 3, 0);
    lv_obj_set_style_clip_corner(s_eye_right, true, 0);
    lv_obj_set_style_pad_all(s_eye_right, 0, 0);

    /* Linea horizontal en el centro del ojo derecho para el parpado */
    s_eyelid_line_right = lv_obj_create(s_eye_right);
    lv_obj_remove_flag(s_eyelid_line_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_eyelid_line_right, EYELID_LINE_W, EYELID_LINE_H);
    lv_obj_set_pos(s_eyelid_line_right, (EYE_SIZE - EYELID_LINE_W) / 2, (EYE_SIZE - EYELID_LINE_H) / 2);
    lv_obj_set_style_radius(s_eyelid_line_right, 2, 0);
    lv_obj_set_style_bg_color(s_eyelid_line_right, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_bg_opa(s_eyelid_line_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_eyelid_line_right, 0, 0);
    lv_obj_set_style_pad_all(s_eyelid_line_right, 0, 0);
    lv_obj_add_flag(s_eyelid_line_right, LV_OBJ_FLAG_HIDDEN);

    /* Iris derecho mas angosto */
    s_iris_right = lv_obj_create(s_eye_right);
    lv_obj_remove_flag(s_iris_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_iris_right, IRIS_SIZE, IRIS_SIZE);
    lv_obj_set_pos(s_iris_right, (EYE_SIZE - IRIS_SIZE) / 2, (EYE_SIZE - IRIS_SIZE) / 2);
    lv_obj_set_style_radius(s_iris_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_iris_right, lv_color_hex(COLOR_IRIS), 0);
    lv_obj_set_style_bg_opa(s_iris_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_iris_right, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_border_width(s_iris_right, 2, 0);
    lv_obj_set_style_clip_corner(s_iris_right, true, 0);
    lv_obj_set_style_pad_all(s_iris_right, 0, 0);

    /* Pupila derecha bastante grande */
    s_pupil_right = lv_obj_create(s_iris_right);
    lv_obj_remove_flag(s_pupil_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_pupil_right, PUPIL_SIZE, PUPIL_SIZE);
    lv_obj_set_pos(s_pupil_right, (IRIS_SIZE - PUPIL_SIZE) / 2, (IRIS_SIZE - PUPIL_SIZE) / 2);
    lv_obj_set_style_radius(s_pupil_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pupil_right, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_bg_opa(s_pupil_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_pupil_right, 0, 0);
    lv_obj_set_style_clip_corner(s_pupil_right, true, 0);
    lv_obj_set_style_pad_all(s_pupil_right, 0, 0);

    /* Unico reflejo blanco en la pupila derecha */
    s_shine_right = lv_obj_create(s_pupil_right);
    lv_obj_remove_flag(s_shine_right, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_shine_right, SHINE_SIZE, SHINE_SIZE);
    lv_obj_set_pos(s_shine_right, 14, 10);
    lv_obj_set_style_radius(s_shine_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_shine_right, lv_color_hex(COLOR_WHITE), 0);
    lv_obj_set_style_bg_opa(s_shine_right, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_shine_right, 0, 0);
    lv_obj_set_style_pad_all(s_shine_right, 0, 0);

    /* Boca para animacion de habla */
    s_mouth = lv_obj_create(screen);
    lv_obj_remove_flag(s_mouth, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_mouth, 24, 12);
    lv_obj_set_pos(s_mouth, FACE_CENTER_X - 12, MOUTH_CENTER_Y - 6);
    lv_obj_set_style_radius(s_mouth, 6, 0);
    lv_obj_set_style_bg_color(s_mouth, lv_color_hex(COLOR_PUPIL), 0);
    lv_obj_set_style_bg_opa(s_mouth, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_mouth, 0, 0);
    lv_obj_set_style_clip_corner(s_mouth, true, 0);
    lv_obj_set_style_pad_all(s_mouth, 0, 0);
    lv_obj_add_flag(s_mouth, LV_OBJ_FLAG_HIDDEN);

    /* Lengua interior rosa (#F8959F) para habla */
    s_tongue = lv_obj_create(s_mouth);
    lv_obj_remove_flag(s_tongue, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tongue, 16, 8);
    lv_obj_set_pos(s_tongue, 4, 5);
    lv_obj_set_style_radius(s_tongue, 4, 0);
    lv_obj_set_style_bg_color(s_tongue, lv_color_hex(COLOR_IRIS), 0);
    lv_obj_set_style_bg_opa(s_tongue, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_tongue, 0, 0);
    lv_obj_set_style_pad_all(s_tongue, 0, 0);

    /*
     * Sonrisa principal de Las Chicas Superpoderosas:
     * Arco anti-aliased en U con extremos redondeados (3px de grosor).
     */
    s_smile_arc = lv_arc_create(screen);
    lv_obj_remove_flag(s_smile_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_smile_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_arc_set_mode(s_smile_arc, LV_ARC_MODE_NORMAL);
    lv_arc_set_bg_angles(s_smile_arc, 20, 160);
    lv_obj_set_size(s_smile_arc, 24, 24);
    lv_obj_set_pos(s_smile_arc, FACE_CENTER_X - 12, MOUTH_CENTER_Y - 12);
    lv_obj_set_style_bg_opa(s_smile_arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_smile_arc, 0, 0);
    lv_obj_set_style_pad_all(s_smile_arc, 0, 0);
    lv_obj_set_style_arc_color(s_smile_arc, lv_color_hex(COLOR_PUPIL), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_smile_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_smile_arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_smile_arc, LV_OPA_0, LV_PART_INDICATOR);
    lv_obj_set_style_opa(s_smile_arc, LV_OPA_0, LV_PART_KNOB);

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
