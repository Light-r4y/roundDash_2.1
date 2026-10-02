#pragma once
#include <stdbool.h>

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
