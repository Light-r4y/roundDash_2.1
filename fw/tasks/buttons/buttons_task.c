#include "buttons_task.h"
#include "conf.h"
#include "board.h"
#include "webcfg_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUTTONS";

static void buttons_task(void *arg)
{
    ESP_LOGI(TAG, "Buttons task started");

    TickType_t last = xTaskGetTickCount();
    uint32_t btn1_held_ms = 0;

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RG_BUTTONS_TICK_MS));

        // Долгое нажатие кнопки 1 поднимает точку доступа - один раз за нажатие.
        if (roundGauge_board_btn_pressed(0)) {
            if (btn1_held_ms < RG_BTN_AP_HOLD_MS) {
                btn1_held_ms += RG_BUTTONS_TICK_MS;
                if (btn1_held_ms >= RG_BTN_AP_HOLD_MS && !roundGauge_webcfg_running()) {
                    ESP_LOGI(TAG, "Button 1 held - starting access point");
                    roundGauge_webcfg_start();
                }
            }
        } else {
            btn1_held_ms = 0;
        }

        // TODO: подавление дребезга, короткое/длинное нажатие обеих кнопок и
        //       события для ui_task — когда будет решено, что делают кнопки.
    }
}

void roundGauge_buttons_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(buttons_task, "buttons_task", RG_BUTTONS_TASK_STACK, NULL,
                                            RG_BUTTONS_TASK_PRIORITY, NULL, RG_BUTTONS_TASK_CORE);
    configASSERT(ok == pdPASS);
}
