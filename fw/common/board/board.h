/*
 * board — единственное место в проекте, где встречаются номера GPIO.
 *
 * Плата: Waveshare ESP32-S3-Touch-LCD-2.1 (ESP32-S3R8, 16 МБ флеш, 8 МБ PSRAM),
 * схема: hw/board/ESP32-S3-Touch-LCD-2.1_schematic_diagram.pdf, даташиты — hw/datasheets/
 *
 * Почти все GPIO заняты RGB-экраном. Свободные выведены на 12-пиновый разъём
 * J9 «12PIN Multi-function Interface» (SH1.0), нумерация по схеме:
 *
 *   Пин  Сигнал        Назначение в проекте
 *   1    GND
 *   2    5V (USB_5V)   питание от DC-DC 12→5 В
 *   3    GPIO19 (D-)   CAN RX  ← RXD трансивера
 *   4    GPIO20 (D+)   CAN TX  → TXD трансивера
 *   5    GND
 *   6    3V3           питание трансивера
 *   7    GPIO7  SCL    I2C платы, резерв
 *   8    GPIO15 SDA    I2C платы, резерв
 *   9    GPIO43 TXD    логи (консоль UART0)
 *   10   GPIO44 RXD    кнопка 2
 *   11   GND
 *   12   GPIO0         кнопка 1 (параллельно BOOT)
 *
 * Три особенности платы, из которых выросла раскладка:
 *
 *  1. Type-C на плате идёт через CH343 на UART0, а не на встроенный USB.
 *     Поэтому GPIO19/20 свободны, и CAN на них не мешает ни прошивке, ни логам.
 *
 *  2. Переключатель FSUSB42 отдаёт GPIO43/44 то CH343, то разъёму J9, и
 *     управляется напряжением VBUS на Type-C. Пока Type-C под питанием, кнопка 2
 *     от разъёма отключена. В машине плату нужно питать через пин 2 разъёма, не
 *     через Type-C (диоды на плате не пустят 5 В обратно в USB компьютера).
 *
 *  3. GPIO0 — strapping-пин и кнопка BOOT. Кнопка 1, зажатая в момент включения,
 *     переводит плату в режим прошивки. На плате есть подтяжка 10 кОм.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------
// Разъём J9: CAN и кнопки
// ------------------------------------------------------------------
#define RG_PIN_CAN_TX GPIO_NUM_20 /* J9 пин 4 */
#define RG_PIN_CAN_RX GPIO_NUM_19 /* J9 пин 3 */

#define RG_PIN_BTN1 GPIO_NUM_0  /* J9 пин 12, strapping, параллельно BOOT */
#define RG_PIN_BTN2 GPIO_NUM_44 /* J9 пин 10, UART0 RX, через FSUSB42 */

// ------------------------------------------------------------------
// I2C платы: тач, TCA9554, RTC, IMU (выведен и на J9 пины 7/8)
// ------------------------------------------------------------------
#define RG_PIN_I2C_SCL GPIO_NUM_7
#define RG_PIN_I2C_SDA GPIO_NUM_15

// Занятые адреса по FAQ Waveshare: 0x15, 0x20, 0x51, 0x6B и 0x7E (чем занят
// последний, в FAQ не сказано). Внешние устройства на J9 — только на свободные.
#define RG_I2C_ADDR_TOUCH 0x15    /* CST820 */
#define RG_I2C_ADDR_EXPANDER 0x20 /* TCA9554 */
#define RG_I2C_ADDR_RTC 0x51      /* PCF85063 */
#define RG_I2C_ADDR_IMU 0x6B      /* QMI8658 */

// ------------------------------------------------------------------
// TCA9554: все 8 выводов заняты платой
// ------------------------------------------------------------------
#define RG_EXIO_LCD_RST 0   /* EXIO1 */
#define RG_EXIO_TP_RST 1    /* EXIO2 */
#define RG_EXIO_LCD_CS 2    /* EXIO3 */
#define RG_EXIO_SD_CS 3     /* EXIO4 */
#define RG_EXIO_IMU_INT2 4  /* EXIO5 */
#define RG_EXIO_IMU_INT1 5  /* EXIO6 */
#define RG_EXIO_RTC_INT 6   /* EXIO7 */
#define RG_EXIO_BUZZER 7    /* EXIO8, зуммер (roundGauge_board_buzzer_set) */

// ------------------------------------------------------------------
// Экран ST7701S, RGB565, 480×480
// ------------------------------------------------------------------
// 3-wire SPI для init-последовательности ST7701 (делит ножки с SD-картой).
#define RG_PIN_LCD_SPI_SDA GPIO_NUM_1
#define RG_PIN_LCD_SPI_SCL GPIO_NUM_2

#define RG_PIN_LCD_PCLK GPIO_NUM_41
#define RG_PIN_LCD_DE GPIO_NUM_40
#define RG_PIN_LCD_VSYNC GPIO_NUM_39
#define RG_PIN_LCD_HSYNC GPIO_NUM_38
#define RG_PIN_LCD_BL GPIO_NUM_6

// Шина данных в порядке esp_lcd (D0..D15 = B0..B4, G0..G5, R0..R4). На
// разъёме панели это B1-B5, G0-G5, R1-R5: R0/B0 панели не разведены.
#define RG_PIN_LCD_DATA                                      \
    {                                                        \
        GPIO_NUM_5, GPIO_NUM_45, GPIO_NUM_48, GPIO_NUM_47,   \
        GPIO_NUM_21, GPIO_NUM_14, GPIO_NUM_13, GPIO_NUM_12,  \
        GPIO_NUM_11, GPIO_NUM_10, GPIO_NUM_9, GPIO_NUM_46,   \
        GPIO_NUM_3, GPIO_NUM_8, GPIO_NUM_18, GPIO_NUM_17,    \
    }

#define RG_LCD_H_RES 480
#define RG_LCD_V_RES 480

#define RG_PIN_TOUCH_INT GPIO_NUM_16

// ------------------------------------------------------------------
// Прочее
// ------------------------------------------------------------------
#define RG_PIN_SD_MISO GPIO_NUM_42 /* MOSI/SCK — общие с RG_PIN_LCD_SPI_* */
#define RG_PIN_BAT_ADC GPIO_NUM_4

// Инициализация кнопок, I2C, TCA9554, экрана и тача. Подсветка после неё
// выключена: её включает ui_task, когда первый кадр уже нарисован.
esp_err_t roundGauge_board_init(void);

// Сырое состояние кнопки, без подавления дребезга. idx: 0 — кнопка 1, 1 — кнопка 2.
bool roundGauge_board_btn_pressed(int idx);

// Панель RGB с двумя кадровыми буферами в PSRAM (для esp_lvgl_port).
esp_lcd_panel_handle_t roundGauge_board_lcd_panel(void);

// Тач CST820 (драйвер CST816S), прерывание на RG_PIN_TOUCH_INT. Без тача (RG_HAS_TOUCH=0)
// не инициализируется, возвращает NULL.
esp_lcd_touch_handle_t roundGauge_board_touch(void);

// Общая шина I2C платы (тач, TCA9554, RTC, IMU): устройства на ней добавляют сами пользователи.
i2c_master_bus_handle_t roundGauge_board_i2c(void);

// Яркость подсветки, 0-100 %.
esp_err_t roundGauge_board_backlight_set(uint8_t pct);

// Зуммер на выводе EXIO8 расширителя TCA9554: включить или выключить (простое включение, тонов нет;
// звучит ли непрерывно или нужны импульсы - зависит от зуммера: активный пищит сам).
esp_err_t roundGauge_board_buzzer_set(bool on);

#ifdef __cplusplus
}
#endif
