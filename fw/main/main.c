/*
 * roundGauge — приборка на Waveshare ESP32-S3-Touch-LCD-2.1: данные по CAN,
 * настройка по Wi-Fi, две кнопки на разъёме J9.
 *
 * Устройство прошивки: docs/fw-design.md
 */
#include <string.h>
#include "nvs_flash.h"
#include "esp_log.h"
#include "board.h"
#include "settings.h"
#include "auth.h"
#include "ui_task.h"
#include "can_task.h"
#include "buttons_task.h"
#include "webcfg_task.h"

// RG_LOG_LEVEL_STR задаётся в CMakeLists.txt: "INFO" при обычном "idf.py build",
// "WARN" — в релизной сборке через tools/build/build.*.
static void apply_log_level(void)
{
    if (strcmp(RG_LOG_LEVEL_STR, "WARN") == 0) {
        esp_log_level_set("*", ESP_LOG_WARN);
    }
}

// До любых задач, читающих NVS: иначе первая же из них получит
// ESP_ERR_NVS_NOT_INITIALIZED.
static void nvs_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
}

void app_main(void)
{
    apply_log_level();
    nvs_init();

    // Синхронно, до задач: все они читают настройки с первого же такта.
    roundGauge_settings_init();
    roundGauge_auth_init();

    ESP_ERROR_CHECK(roundGauge_board_init());

    roundGauge_ui_task_start();      // Отрисовка, ядро 1 (вместе с задачей esp_lvgl_port)
    roundGauge_can_task_start();     // Приём и разбор CAN, ядро 0
    roundGauge_webcfg_task_start();  // Точка доступа по запросу, ядро 0

    // Последней: долгое нажатие сразу же обращается к webcfg_task.
    roundGauge_buttons_task_start(); // Опрос кнопок, ядро 0
}
