#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "conf.h"

// Правила звуковых тревог. Хранятся в NVS отдельным blob'ом (RG_ALERTS_NVS_KEY) в виде JSON, в памяти -
// разобранной структурой. Правило: когда значение сигнала (по имени из таблицы сигналов layout)
// выходит за порог, зуммер звучит по заданному узору.
//
// {
//   "version": 1,
//   "rules": [
//     { "signal": "coolant", "op": ">", "value": 105, "hyst": 2,
//       "pattern": "beeps", "count": 3, "on": 120, "off": 120, "repeat": 5, "enabled": true },
//     { "signal": "oil_press", "op": "<", "value": 1.0, "pattern": "continuous" }
//   ]
// }
//
// "op" - ">" или "<". "hyst" - гистерезис: тревога снимается, когда значение вернулось за порог на hyst
// (по умолчанию 0). "pattern": "beeps" - серия из count писков (1..9) по on мс с паузами off мс, повтор
// серии каждые repeat с (0 - один раз за срабатывание); "continuous" - непрерывный сигнал, пока
// условие выполняется. Нет данных у сигнала - тревоги нет.

#define RG_ALERTS_MAX 8

typedef enum {
    RG_ALERT_BEEPS = 0,
    RG_ALERT_CONTINUOUS,
} roundGauge_alert_pattern_t;

typedef struct {
    char signal[16];
    char op; // '>' или '<'
    float value;
    float hyst;
    roundGauge_alert_pattern_t pattern;
    uint8_t count;     // beeps: писков в серии, 1..9
    uint16_t on_ms;    // beeps: длина писка, 20..2000
    uint16_t off_ms;   // beeps: пауза между писками, 20..2000
    uint16_t repeat_s; // beeps: повтор серии, с (0 - один раз)
    bool enabled;
} roundGauge_alert_rule_t;

typedef struct {
    uint8_t count;
    roundGauge_alert_rule_t r[RG_ALERTS_MAX];
} roundGauge_alerts_t;

// Читает правила из NVS; нет сохранённых (или битые) - встроенные из alerts_default.h.
// После nvs_flash_init().
void roundGauge_alerts_init(void);

// Копия текущих правил.
void roundGauge_alerts_copy(roundGauge_alerts_t *out);

// Разобрать JSON, проверить, сохранить в NVS и сделать текущими. ESP_ERR_INVALID_ARG - JSON не
// разобрался или нет ни одного годного правила при непустом "rules" (текущие остаются).
// Пустой "rules" допустим - все правила удаляются.
esp_err_t roundGauge_alerts_apply_json(const char *json);

// JSON текущих правил в куче (освободить free()); NULL при нехватке памяти.
char *roundGauge_alerts_json_dup(void);

// Стереть сохранённые и вернуть встроенные.
esp_err_t roundGauge_alerts_reset(void);

// Растёт при каждой смене правил: sound_task по нему перечитывает таблицу.
uint32_t roundGauge_alerts_generation(void);
