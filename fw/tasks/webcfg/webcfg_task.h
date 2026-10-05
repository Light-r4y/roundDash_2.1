#pragma once
#include <stdbool.h>
#include <stdint.h>

// Настройка по Wi-Fi: точка доступа + HTTP-сервер.
//
// В обычной езде Wi-Fi выключен. Точку доступа поднимает долгое нажатие
// кнопки 1 (buttons_task), гаснет она только с перезагрузкой. SSID
// roundGauge-XXXX (хвост MAC) или заданный в настройках, WPA2.
//
// HTTP: статика из раздела www (RG_WWW_BASE_PATH), страница /ota вшита в
// прошивку (ota_page.h), изменяющие запросы и OTA закрыты необязательным
// паролем (common/config/auth.h).

void roundGauge_webcfg_task_start(void);

// Запросить подъём точки доступа. Не блокирует и потокобезопасно: запрос
// выполнит webcfg_task в течение RG_WEBCFG_TICK_MS.
void roundGauge_webcfg_start(void);

// true, если точка доступа уже поднята (или поднимается).
bool roundGauge_webcfg_running(void);

// Состояние точки доступа для экрана: выключена / запускается (кнопка нажата, сеть ещё
// не поднята) / работает. Пока сеть открыта, пароль пустой.
typedef enum {
    RG_AP_OFF = 0,
    RG_AP_STARTING,
    RG_AP_UP,
} roundGauge_ap_state_t;

typedef struct {
    roundGauge_ap_state_t state;
    char ssid[33];
    char password[65];
    uint8_t clients; // сколько устройств подключено сейчас
} roundGauge_ap_info_t;

void roundGauge_webcfg_get_ap_info(roundGauge_ap_info_t *out);
