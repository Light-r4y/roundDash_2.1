#include "buttons_task.h"
#include "conf.h"
#include "board.h"
#include "webcfg_task.h"
#include "ui_task.h"
#include "auth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUTTONS";

static void buttons_task(void *arg)
{
    ESP_LOGI(TAG, "Buttons task started");

    TickType_t last = xTaskGetTickCount();
    uint32_t btn1_held_ms = 0;
    bool ap_fired = false; // для этого нажатия точка доступа уже запущена
    bool reset_fired = false;
    uint32_t btn2_down_ticks = 0;

    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RG_BUTTONS_TICK_MS));

        // Кнопка 1 (BOOT, GPIO0). Долгое нажатие поднимает точку доступа - один раз за
        // нажатие; короткое (отпущена раньше) переключает экран. Эта кнопка работает и
        // при питании по Type-C, в отличие от кнопки 2 (GPIO44 занят USB-UART).
        if (roundGauge_board_btn_pressed(0)) {
            if (btn1_held_ms < RG_AUTH_RESET_HOLD_MS) {
                btn1_held_ms += RG_BUTTONS_TICK_MS;
            }
            if (btn1_held_ms >= RG_BTN_AP_HOLD_MS && !ap_fired) {
                ap_fired = true;
                if (!roundGauge_webcfg_running()) {
                    ESP_LOGI(TAG, "Button 1 held - starting access point");
                    roundGauge_webcfg_start();
                }
            }
            // Дальше держим - снимаем забытый пароль на настройки (консоли для этого нет).
            if (btn1_held_ms >= RG_AUTH_RESET_HOLD_MS && !reset_fired) {
                reset_fired = true;
                bool had = roundGauge_auth_is_enabled();
                roundGauge_auth_set_password(NULL);
                ESP_LOGW(TAG, "Button 1 held %d ms - web password cleared", RG_AUTH_RESET_HOLD_MS);
                roundGauge_ui_toast(had ? "Password reset" : "No password set");
            }
        } else {
            if (btn1_held_ms >= RG_BTN_DEBOUNCE_TICKS * RG_BUTTONS_TICK_MS && btn1_held_ms < RG_BTN_AP_HOLD_MS) {
                roundGauge_ui_switch_screen(+1);
            }
            btn1_held_ms = 0;
            ap_fired = false;
            reset_fired = false;
        }

        // Кнопка 2: нажатие (после подавления дребезга) переключает экран.
        // Срабатывает один раз за нажатие, пока кнопку держат - повтора нет.
        if (roundGauge_board_btn_pressed(1)) {
            if (btn2_down_ticks < RG_BTN_DEBOUNCE_TICKS && ++btn2_down_ticks == RG_BTN_DEBOUNCE_TICKS) {
                roundGauge_ui_switch_screen(+1);
            }
        } else {
            btn2_down_ticks = 0;
        }

        // TODO: длинное нажатие кнопки 2 - когда будет решено, что оно делает.
    }
}

void roundGauge_buttons_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(buttons_task, "buttons_task", RG_BUTTONS_TASK_STACK, NULL,
                                            RG_BUTTONS_TASK_PRIORITY, NULL, RG_BUTTONS_TASK_CORE);
    configASSERT(ok == pdPASS);
}
