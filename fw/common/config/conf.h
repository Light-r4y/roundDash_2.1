#pragma once

// Константы проекта. Номера GPIO сюда не попадают — они живут только в
// common/board/board.h.

// ------------------------------------------------------------------
// Задачи: стек, приоритет, ядро (docs/fw-design.md §2)
// ------------------------------------------------------------------
// Ядро 1 целиком под отрисовку, всё остальное — на ядре 0 вместе с Wi-Fi.
// Wi-Fi включается только в режиме настройки, поэтому в обычной езде CAN и
// кнопки ядро 0 ни с кем не делят.

// Задача esp_lvgl_port: крутит lv_timer_handler и рендер.
#define RG_LVGL_TASK_STACK 8192
#define RG_LVGL_TASK_PRIORITY 4
#define RG_LVGL_TASK_CORE 1

// ui_task: переносит значения сигналов в виджеты под lvgl_port_lock().
#define RG_UI_TASK_STACK 4096
#define RG_UI_TASK_PRIORITY 3
#define RG_UI_TASK_CORE 1
#define RG_UI_TASK_PERIOD_MS 33

#define RG_CAN_TASK_STACK 4096
#define RG_CAN_TASK_PRIORITY 8
#define RG_CAN_TASK_CORE 0

#define RG_BUTTONS_TASK_STACK 2560
#define RG_BUTTONS_TASK_PRIORITY 5
#define RG_BUTTONS_TASK_CORE 0
#define RG_BUTTONS_TICK_MS 10

// webcfg_task: подъём точки доступа и HTTP-сервера по запросу.
#define RG_WEBCFG_TASK_STACK 4096
#define RG_WEBCFG_TASK_PRIORITY 2
#define RG_WEBCFG_TASK_CORE 0
#define RG_WEBCFG_TICK_MS 500

// Задачу httpd создаёт esp_http_server, сюда — только её параметры.
#define RG_HTTPD_STACK 8192
#define RG_HTTPD_PRIORITY 5
#define RG_HTTPD_CORE 0

// ------------------------------------------------------------------
// Экран
// ------------------------------------------------------------------
// Яркость подсветки после старта, %. Регулировки пока нет.
#define RG_LCD_BL_DEFAULT_PCT 100

// ------------------------------------------------------------------
// Кнопки
// ------------------------------------------------------------------
#define RG_BTN_COUNT 2
// Удержание кнопки 1 поднимает точку доступа. Гаснет она только с перезагрузкой.
#define RG_BTN_AP_HOLD_MS 2000

// ------------------------------------------------------------------
// CAN
// ------------------------------------------------------------------
// Кадры из ISR идут в can_task через очередь.
// TODO: подобрать длину по реальной загрузке шины.
#define RG_CAN_RX_QUEUE_LEN 32

// Значения по умолчанию, пока в NVS пусто: новая приборка ничего не передаёт в
// шину, пока её не настроят.
#define RG_CAN_BITRATE_DEFAULT 500000
#define RG_CAN_LISTEN_ONLY_DEFAULT true

// ------------------------------------------------------------------
// Wi-Fi: точка доступа поднимается только кнопкой
// ------------------------------------------------------------------
// SSID по умолчанию — префикс + хвост MAC (roundGauge-XXXX). SSID и пароль
// меняются через веб и хранятся в NVS.
#define RG_WIFI_AP_SSID_PREFIX "roundGauge-"
#define RG_WIFI_AP_PASS_DEFAULT "roundgauge"
#define RG_WIFI_AP_SSID_MAX_LEN 32
// WPA2-PSK требует пароль длиной 8-64 символа.
#define RG_WIFI_AP_PASSWORD_MIN_LEN 8
#define RG_WIFI_AP_PASSWORD_MAX_LEN 64

// ------------------------------------------------------------------
// Разделы SPIFFS (partitions.csv)
// ------------------------------------------------------------------
#define RG_WWW_PARTITION "www"
#define RG_WWW_BASE_PATH "/www"
#define RG_MEDIA_PARTITION "media"
#define RG_MEDIA_BASE_PATH "/media"

// ------------------------------------------------------------------
// Настройки в NVS (common/config/settings.c)
// ------------------------------------------------------------------
// JSON целиком одним blob'ом. Добавление полей старые настройки не сбрасывает:
// чего нет в JSON, то берётся из значений по умолчанию.
#define RG_SETTINGS_NVS_NAMESPACE "rg_cfg"
#define RG_SETTINGS_NVS_KEY "cfg"
#define RG_SETTINGS_VERSION 1

// ------------------------------------------------------------------
// Пароль на OTA и изменяющие запросы API (common/config/auth.c)
// ------------------------------------------------------------------
#define RG_AUTH_NVS_KEY "auth_hash"          // SHA-256(salt + пароль), пусто/отсутствует = защита выключена
#define RG_AUTH_SALT "roundGauge-auth-v1-"   // фиксированная "соль", вкомпилирована в прошивку
#define RG_AUTH_REALM "roundGauge"           // realm для Basic Auth (что видит браузер в диалоге)
