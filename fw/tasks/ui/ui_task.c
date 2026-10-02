#include "ui_task.h"
#include "conf.h"
#include "board.h"
#include "esp_lvgl_port.h"
#include "test_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "UI";

static lv_display_t *s_disp;

static void lvgl_start(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = RG_LVGL_TASK_PRIORITY,
        .task_stack = RG_LVGL_TASK_STACK,
        .task_affinity = RG_LVGL_TASK_CORE,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    /*
     * Без разрывов: LVGL рисует прямо в кадровые буферы панели (два, в PSRAM)
     * в direct mode, esp_lvgl_port меняет их по окончании кадра. Буферы
     * выделяет драйвер панели (board.c), поэтому buffer_size — целый экран.
     */
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = roundGauge_board_lcd_panel(),
        .buffer_size = RG_LCD_H_RES * RG_LCD_V_RES,
        .double_buffer = true,
        .hres = RG_LCD_H_RES,
        .vres = RG_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .direct_mode = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    configASSERT(s_disp != NULL);

    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = s_disp,
        .handle = roundGauge_board_touch(),
    };
    configASSERT(lvgl_port_add_touch(&touch_cfg) != NULL);
}

// TODO: смонтировать раздел media (RG_MEDIA_PARTITION -> RG_MEDIA_BASE_PATH) и
//       подключить к нему файловый драйвер LVGL; экраны приборки вместо тестового.

static void ui_task(void *arg)
{
    lvgl_start();

    lvgl_port_lock(0);
    test_screen_create();
    lvgl_port_unlock();

    // Подсветка после первого кадра — без мелькания мусора из PSRAM.
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_ERROR_CHECK(roundGauge_board_backlight_set(RG_LCD_BL_DEFAULT_PCT));
    ESP_LOGI(TAG, "UI task started, test screen");

    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RG_UI_TASK_PERIOD_MS));

        // TODO: под lvgl_port_lock() перенести значения сигналов в виджеты,
        //       обработать события кнопок (переключение экранов).
    }
}

void roundGauge_ui_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui_task", RG_UI_TASK_STACK, NULL,
                                            RG_UI_TASK_PRIORITY, NULL, RG_UI_TASK_CORE);
    configASSERT(ok == pdPASS);
}
