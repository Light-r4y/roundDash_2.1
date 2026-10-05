#include "board.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_io_expander_tca9554.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_io_additions.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_st7701.h"
#include "esp_lcd_touch_cst816s.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "st7701_init.h"

static const char *TAG = "BOARD";

static const gpio_num_t s_btn_pins[] = { RG_PIN_BTN1, RG_PIN_BTN2 };

static i2c_master_bus_handle_t s_i2c_bus;
static esp_io_expander_handle_t s_expander;
static esp_lcd_panel_handle_t s_lcd_panel;
static esp_lcd_touch_handle_t s_touch;

// ------------------------------------------------------------------
// Параметры панели. Тайминги, частота PCLK и размер bounce-буфера — из
// конфигурации платы от Waveshare (см. st7701_init.h).
// ------------------------------------------------------------------
#define LCD_PCLK_HZ (16 * 1000 * 1000)
#define LCD_HSYNC_PULSE_WIDTH 8
#define LCD_HSYNC_BACK_PORCH 10
#define LCD_HSYNC_FRONT_PORCH 50
#define LCD_VSYNC_PULSE_WIDTH 3
#define LCD_VSYNC_BACK_PORCH 8
#define LCD_VSYNC_FRONT_PORCH 8

// Bounce-буфер во внутренней RAM: DMA берёт кадр не напрямую из PSRAM, и
// картинка не съезжает, когда PSRAM занята кем-то ещё (Wi-Fi, запись флеша).
#define LCD_BOUNCE_BUFFER_PX (RG_LCD_H_RES * 10)

// Два кадровых буфера: esp_lvgl_port рисует в один, пока второй на экране, и
// меняет их по VSYNC — без разрывов картинки.
#define LCD_NUM_FBS 2

#define I2C_CLK_HZ (400 * 1000)

#define BL_LEDC_TIMER LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_FREQ_HZ 5000
#define BL_LEDC_RES LEDC_TIMER_10_BIT
#define BL_LEDC_DUTY_MAX ((1 << 10) - 1)

// ------------------------------------------------------------------

static esp_err_t buttons_init(void)
{
    /*
     * Кнопки замыкают на GND. У GPIO0 есть внешняя подтяжка 10 кОм на плате, у
     * GPIO44 — только внутренняя; для длинных проводов в машине на разъёме
     * нужны свои 10 кОм к 3V3 и 100 нФ на GND.
     *
     * Перевод GPIO44 в обычный вход отключает от него RX UART0 — консоль
     * остаётся только на вывод.
     */
    gpio_config_t btn = {
        .pin_bit_mask = (1ULL << RG_PIN_BTN1) | (1ULL << RG_PIN_BTN2),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    return gpio_config(&btn);
}

static esp_err_t i2c_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = RG_PIN_I2C_SDA,
        .scl_io_num = RG_PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&bus, &s_i2c_bus);
}

// Сброс через TCA9554: активный уровень низкий.
static esp_err_t expander_pulse_reset(uint32_t pin, uint32_t low_ms, uint32_t high_ms)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, pin, 0), TAG, "reset low");
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    ESP_RETURN_ON_ERROR(esp_io_expander_set_level(s_expander, pin, 1), TAG, "reset high");
    vTaskDelay(pdMS_TO_TICKS(high_ms));
    return ESP_OK;
}

static esp_err_t expander_init(void)
{
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_tca9554(s_i2c_bus, RG_I2C_ADDR_EXPANDER, &s_expander),
                        TAG, "tca9554");

    // Только выводы, нужные экрану и тачу; остальные остаются входами, как после сброса.
    const uint32_t outputs = (1U << RG_EXIO_LCD_RST) | (1U << RG_EXIO_TP_RST) | (1U << RG_EXIO_LCD_CS);
    ESP_RETURN_ON_ERROR(esp_io_expander_set_dir(s_expander, outputs, IO_EXPANDER_OUTPUT), TAG, "exio dir");
    return esp_io_expander_set_level(s_expander, outputs, 1);
}

static esp_err_t lcd_init(void)
{
    ESP_RETURN_ON_ERROR(expander_pulse_reset(1U << RG_EXIO_LCD_RST, 10, 100), TAG, "lcd reset");

    // 3-wire SPI только для init-последовательности: SCL/SDA на GPIO, CS на TCA9554.
    spi_line_config_t line_config = {
        .cs_io_type = IO_TYPE_EXPANDER,
        .cs_expander_pin = (esp_io_expander_pin_num_t)(1U << RG_EXIO_LCD_CS),
        .scl_io_type = IO_TYPE_GPIO,
        .scl_gpio_num = RG_PIN_LCD_SPI_SCL,
        .sda_io_type = IO_TYPE_GPIO,
        .sda_gpio_num = RG_PIN_LCD_SPI_SDA,
        .io_expander = s_expander,
    };
    esp_lcd_panel_io_3wire_spi_config_t io_config = ST7701_PANEL_IO_3WIRE_SPI_CONFIG(line_config, 0);
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_3wire_spi(&io_config, &io), TAG, "3wire spi");

    esp_lcd_rgb_panel_config_t rgb_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = LCD_PCLK_HZ,
            .h_res = RG_LCD_H_RES,
            .v_res = RG_LCD_V_RES,
            .hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH,
            .hsync_back_porch = LCD_HSYNC_BACK_PORCH,
            .hsync_front_porch = LCD_HSYNC_FRONT_PORCH,
            .vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH,
            .vsync_back_porch = LCD_VSYNC_BACK_PORCH,
            .vsync_front_porch = LCD_VSYNC_FRONT_PORCH,
            .flags.pclk_active_neg = false,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = LCD_NUM_FBS,
        .bounce_buffer_size_px = LCD_BOUNCE_BUFFER_PX,
        .dma_burst_size = 64,
        .hsync_gpio_num = RG_PIN_LCD_HSYNC,
        .vsync_gpio_num = RG_PIN_LCD_VSYNC,
        .de_gpio_num = RG_PIN_LCD_DE,
        .pclk_gpio_num = RG_PIN_LCD_PCLK,
        .disp_gpio_num = GPIO_NUM_NC,
        .data_gpio_nums = RG_PIN_LCD_DATA,
        .flags.fb_in_psram = true,
    };

    st7701_vendor_config_t vendor_config = {
        .init_cmds = s_st7701_init_cmds,
        .init_cmds_size = sizeof(s_st7701_init_cmds) / sizeof(s_st7701_init_cmds[0]),
        .rgb_config = &rgb_config,
        .flags = {
            .mirror_by_cmd = 1,
            .enable_io_multiplex = 0, // ножки SPI не пересекаются с RGB, панель IO остаётся
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC, // аппаратный сброс уже сделан через TCA9554
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7701(io, &panel_config, &s_lcd_panel), TAG, "st7701");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "panel init");
    return ESP_OK;
}

#if RG_HAS_TOUCH
static esp_err_t touch_init(void)
{
    ESP_RETURN_ON_ERROR(expander_pulse_reset(1U << RG_EXIO_TP_RST, 30, 50), TAG, "touch reset");

    esp_lcd_panel_io_i2c_config_t io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &io_config, &io), TAG, "touch io");

    /*
     * По прерыванию, а не опросом: CST820 засыпает без касания и не отвечает по
     * I2C, а esp_lvgl_port в режиме событий читает тач только после прерывания.
     */
    esp_lcd_touch_config_t tp_config = {
        .x_max = RG_LCD_H_RES,
        .y_max = RG_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, // сброс через TCA9554, сделан выше
        .int_gpio_num = RG_PIN_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 1,
        },
    };
    return esp_lcd_touch_new_i2c_cst816s(io, &tp_config, &s_touch);
}
#endif // RG_HAS_TOUCH

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = BL_LEDC_RES,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "bl timer");

    ledc_channel_config_t channel = {
        .gpio_num = RG_PIN_LCD_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0, // выключена до первого кадра
    };
    return ledc_channel_config(&channel);
}

// ------------------------------------------------------------------

esp_err_t roundGauge_board_init(void)
{
    // Подсветка первой: до неё ножка болтается, и на экране мелькнул бы мусор.
    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");
    ESP_RETURN_ON_ERROR(buttons_init(), TAG, "buttons");
    ESP_RETURN_ON_ERROR(i2c_init(), TAG, "i2c");
    ESP_RETURN_ON_ERROR(expander_init(), TAG, "expander");
    ESP_RETURN_ON_ERROR(lcd_init(), TAG, "lcd");
#if RG_HAS_TOUCH
    ESP_RETURN_ON_ERROR(touch_init(), TAG, "touch");
#endif

    ESP_LOGI(TAG, "init done");
    return ESP_OK;
}

bool roundGauge_board_btn_pressed(int idx)
{
    if (idx < 0 || idx >= (int)(sizeof(s_btn_pins) / sizeof(s_btn_pins[0]))) {
        return false;
    }
    return gpio_get_level(s_btn_pins[idx]) == 0;
}

esp_lcd_panel_handle_t roundGauge_board_lcd_panel(void)
{
    return s_lcd_panel;
}

esp_lcd_touch_handle_t roundGauge_board_touch(void)
{
    return s_touch;
}

esp_err_t roundGauge_board_backlight_set(uint8_t pct)
{
    if (pct > 100) {
        pct = 100;
    }
    uint32_t duty = (uint32_t)BL_LEDC_DUTY_MAX * pct / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, duty), TAG, "bl duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}
