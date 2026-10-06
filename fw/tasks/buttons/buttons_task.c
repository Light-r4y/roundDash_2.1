#include "buttons_task.h"
#include "conf.h"
#include "board.h"
#include "webcfg_task.h"
#include "ui_task.h"
#include "auth.h"
#include "settings.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "BUTTONS";

// Следующий уровень яркости: первый из списка, который меньше текущего; ниже самого низкого - снова самый
// высокий. Текущее значение может быть любым (например, с ползунка в вебе). Сохраняется в настройках,
// как и ползунок, поэтому после включения яркость прежняя.
static void cycle_brightness(void)
{
    static const uint8_t steps[] = RG_BTN2_BRIGHTNESS_STEPS;
    roundGauge_display_settings_t d;
    roundGauge_settings_get_display(&d);
    uint8_t next = steps[0];
    for (size_t i = 0; i < sizeof(steps); i++) {
        if (steps[i] < d.brightness) {
            next = steps[i];
            break;
        }
    }
    d.brightness = next;
    if (roundGauge_settings_set_display(&d) == ESP_OK) {
        roundGauge_board_backlight_set(next);
        ESP_LOGI(TAG, "Brightness %u%%", (unsigned)next);
    }
}

static void buttons_task(void *arg)
{
    ESP_LOGI(TAG, "Buttons task started");

    TickType_t last = xTaskGetTickCount();
    uint32_t btn1_held_ms = 0;
    bool ap_fired = false; // для этого нажатия точка доступа уже запущена
    bool warn_fired = false;
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
            // Дальше держим - снимаем забытый пароль на настройки (консоли для этого нет). Пароль
            // Wi-Fi забыть нельзя: он показан на экране вместе с точкой доступа. Перед сбросом -
            // предупреждение, отпустить кнопку до конца значит отказаться.
            if (btn1_held_ms >= RG_AUTH_RESET_WARN_MS && !warn_fired) {
                warn_fired = true;
                roundGauge_ui_toast("Hold to reset password");
            }
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
            warn_fired = false;
            reset_fired = false;
        }

        // Кнопка 2: нажатие (после подавления дребезга) переключает яркость по кругу
        // (RG_BTN2_BRIGHTNESS_STEPS). Срабатывает один раз за нажатие, пока кнопку держат - повтора нет.
        if (roundGauge_board_btn_pressed(1)) {
            if (btn2_down_ticks < RG_BTN_DEBOUNCE_TICKS && ++btn2_down_ticks == RG_BTN_DEBOUNCE_TICKS) {
                cycle_brightness();
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
