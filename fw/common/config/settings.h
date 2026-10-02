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
//   "can":     { "bitrate": 500000, "mode": "listen_only" },
//   "wifi_ap": { "ssid": "", "password": "roundgauge" }
// }
//
// Пустой ssid означает "roundGauge-XXXX" по MAC — так имя остаётся уникальным
// у каждой приборки, пока его не задали руками.
//
// TODO: таблица сигналов CAN и раскладка экранов — формат ещё не выбран
// (docs/fw-design.md, "Открытые вопросы").

typedef enum {
    RG_CAN_MODE_LISTEN_ONLY = 0, // ничего не передаёт в шину, даже ACK
    RG_CAN_MODE_NORMAL,          // с подтверждением кадров, нужен для запросов OBD-PID
} roundGauge_can_mode_t;

typedef struct {
    uint32_t bitrate;
    roundGauge_can_mode_t mode;
} roundGauge_can_settings_t;

typedef struct {
    char ssid[RG_WIFI_AP_SSID_MAX_LEN + 1];
    char password[RG_WIFI_AP_PASSWORD_MAX_LEN + 1];
} roundGauge_wifi_ap_settings_t;

typedef struct {
    uint32_t version;
    roundGauge_can_settings_t can;
    roundGauge_wifi_ap_settings_t wifi_ap;
} roundGauge_settings_t;

// Читает настройки из NVS, при отсутствии/ошибке разбора берёт значения по
// умолчанию. Вызывать после nvs_flash_init() и до старта задач.
void roundGauge_settings_init(void);

// Текущие настройки. Указатель живёт всё время работы; менять их на лету нельзя —
// новые настройки применяются перезагрузкой.
const roundGauge_settings_t *roundGauge_settings_get(void);

// Заполняет структуру значениями по умолчанию.
void roundGauge_settings_defaults(roundGauge_settings_t *out);

// Сериализует в JSON и пишет в NVS. Применится после перезагрузки.
esp_err_t roundGauge_settings_save(const roundGauge_settings_t *cfg);
