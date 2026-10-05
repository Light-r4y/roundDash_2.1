#include "touch_task.h"
#include <stdlib.h>
#include "conf.h"
#include "board.h"
#include "ui_task.h"
#include "esp_lcd_touch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

#if RG_HAS_TOUCH

static const char *TAG = "TOUCH";

static TaskHandle_t s_task;

static void IRAM_ATTR touch_isr(esp_lcd_touch_handle_t tp)
{
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(s_task, &woken);
    portYIELD_FROM_ISR(woken);
}

// Читает контроллер; true - палец на экране. Спящий CST820 на I2C не отвечает, поэтому
// ошибка чтения - это "касания нет", а не повод падать.
static bool read_point(esp_lcd_touch_handle_t tp, int *x, int *y)
{
    if (esp_lcd_touch_read_data(tp) != ESP_OK) {
        return false;
    }
    esp_lcd_touch_point_data_t p[1];
    uint8_t n = 0;
    if (esp_lcd_touch_get_data(tp, p, &n, 1) != ESP_OK || n == 0) {
        return false;
    }
    *x = p[0].x;
    *y = p[0].y;
    return true;
}

static void touch_task(void *arg)
{
    esp_lcd_touch_handle_t tp = roundGauge_board_touch();
    ESP_ERROR_CHECK(esp_lcd_touch_register_interrupt_callback(tp, touch_isr));
    ESP_LOGI(TAG, "Touch task started (swipe >= %d px within %d ms)", RG_TOUCH_SWIPE_MIN_PX, RG_TOUCH_SWIPE_MAX_MS);

    bool pressed = false;
    bool decided = false; // для этого касания свайп уже сработал или время вышло
    int misses = 0;       // подряд опросов без пальца
    int x0 = 0, y0 = 0;
    int64_t t0 = 0;

    while (1) {
        // Нет касания - спим до прерывания. Палец на экране - просыпаемся и сами: INT
        // при подъёме пальца контроллер может не выдать, а подъём надо заметить.
        ulTaskNotifyTake(pdTRUE, pressed ? pdMS_TO_TICKS(RG_TOUCH_POLL_MS) : portMAX_DELAY);

        int x, y;
        if (read_point(tp, &x, &y)) {
            misses = 0;
            int64_t now = esp_timer_get_time() / 1000;
            if (!pressed) {
                pressed = true;
                decided = false;
                x0 = x;
                y0 = y;
                t0 = now;
            } else if (!decided) {
                int dx = x - x0, dy = y - y0;
                if (now - t0 > RG_TOUCH_SWIPE_MAX_MS) {
                    decided = true; // слишком медленно - это не свайп
                } else if (abs(dx) >= RG_TOUCH_SWIPE_MIN_PX && 2 * abs(dx) >= 3 * abs(dy)) {
                    decided = true;
                    // Палец влево - следующий экран, вправо - предыдущий.
                    roundGauge_ui_switch_screen(dx < 0 ? +1 : -1);
                }
            }
        } else if (pressed && ++misses >= 2) {
            pressed = false; // подъём пальца: два опроса подряд без касания
        }
    }
}

void roundGauge_touch_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(touch_task, "touch_task", RG_TOUCH_TASK_STACK, NULL,
                                            RG_TOUCH_TASK_PRIORITY, &s_task, RG_TOUCH_TASK_CORE);
    configASSERT(ok == pdPASS);
}

#else // экран без тача: задачи нет

void roundGauge_touch_task_start(void)
{
}

#endif // RG_HAS_TOUCH
