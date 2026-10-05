#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "conf.h"

// Общие настройки приборки. В NVS лежат JSON-blob'ом (RG_SETTINGS_NVS_KEY),
// в памяти — разобранной структурой; после разбора JSON не хранится.
//
// {
//   "version": 1,
//   "can":     { "bitrate": 500000, "mode": "listen_only", "demo": false },
//   "display": { "brightness": 100 },
//   "wifi_ap": { "ssid": "", "password": "roundgauge" }
// }
//
// Пустой ssid означает "roundGauge-XXXX" по MAC — так имя остаётся уникальным
// у каждой приборки, пока его не задали руками.
//
// Раскладка экранов и таблица сигналов лежат отдельно: layout.h (NVS "layout"), привязки
// сигналов к кадрам CAN - can_map.h (NVS "can_map"). Формат - в docs/fw-design.md §6 и §7.

typedef enum {
    RG_CAN_MODE_LISTEN_ONLY = 0, // ничего не передаёт в шину, даже ACK
    RG_CAN_MODE_NORMAL,          // с подтверждением кадров, нужен для запросов OBD-PID
} roundGauge_can_mode_t;

typedef struct {
    uint32_t bitrate;
    roundGauge_can_mode_t mode;
    bool demo; // сигналы гонит генератор вместо шины (для настройки вида на столе)
} roundGauge_can_settings_t;

typedef struct {
    char ssid[RG_WIFI_AP_SSID_MAX_LEN + 1];
    char password[RG_WIFI_AP_PASSWORD_MAX_LEN + 1];
} roundGauge_wifi_ap_settings_t;

typedef struct {
    uint8_t brightness; // подсветка, % (RG_LCD_BL_MIN_PCT..100)
} roundGauge_display_settings_t;

typedef struct {
    uint32_t version;
    roundGauge_can_settings_t can;
    roundGauge_display_settings_t display;
    roundGauge_wifi_ap_settings_t wifi_ap;
} roundGauge_settings_t;

// Читает настройки из NVS, при отсутствии/ошибке разбора берёт значения по
// умолчанию. Вызывать после nvs_flash_init() и до старта задач.
void roundGauge_settings_init(void);

// Текущие настройки. Указатель живёт всё время работы; менять через него нельзя.
// Wi-Fi (wifi_ap) применяется перезагрузкой; раздел can - на лету через
// roundGauge_settings_set_can() / roundGauge_settings_get_can().
const roundGauge_settings_t *roundGauge_settings_get(void);

// Копия раздела can под защитой от одновременной записи из веба.
void roundGauge_settings_get_can(roundGauge_can_settings_t *out);

// Заменить раздел can и сохранить в NVS. Узел CAN перенастраивается отдельно
// (roundGauge_can_apply_settings()).
esp_err_t roundGauge_settings_set_can(const roundGauge_can_settings_t *can);

// То же для раздела display (яркость): применяется сразу, без перезагрузки.
void roundGauge_settings_get_display(roundGauge_display_settings_t *out);
esp_err_t roundGauge_settings_set_display(const roundGauge_display_settings_t *d);

// Заполняет структуру значениями по умолчанию.
void roundGauge_settings_defaults(roundGauge_settings_t *out);

// Сериализует в JSON и пишет в NVS. Применится после перезагрузки.
esp_err_t roundGauge_settings_save(const roundGauge_settings_t *cfg);
