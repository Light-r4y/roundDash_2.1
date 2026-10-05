#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "conf.h"

// Таблица привязки сигналов к кадрам CAN. Хранится в NVS отдельным blob'ом
// (RG_CAN_MAP_NVS_KEY) в виде JSON, в памяти - разобранной структурой. Каждая запись
// говорит, как из кадра достать значение сигнала из таблицы сигналов layout
// (ссылка по имени); поля - как в DBC, значение = raw * factor + offset.
//
// {
//   "version": 1,
//   "map": [
//     { "signal": "rpm", "id": "0x201", "ext": false,
//       "start": 7, "len": 16, "order": "motorola", "signed": false,
//       "factor": 0.25, "offset": 0, "timeout": 2000 }
//   ]
// }
//
// "id" - число или строка ("0x201", "513"). "start" и "order" понимаются ровно как в
// DBC: для "intel" (@1) start - номер младшего бита, для "motorola" (@0) - номер
// старшего бита в нумерации DBC (бит 7 - старший бит нулевого байта, дальше "пила":
// 7..0, 15..8, ...). "timeout" - мс без кадров, после которых сигнал считается
// недействительным (100..60000, по умолчанию 2000). На один сигнал - одна запись.

#define RG_CAN_MAP_MAX 16 // = RG_SIGNAL_MAX_COUNT
#define RG_CAN_TIMEOUT_DEFAULT_MS 2000

typedef enum {
    RG_CAN_ORDER_INTEL = 0,
    RG_CAN_ORDER_MOTOROLA,
} roundGauge_can_order_t;

typedef struct {
    char signal[16];
    uint32_t id;
    bool ext; // 29-битный идентификатор
    uint8_t start;
    uint8_t len;
    roundGauge_can_order_t order;
    bool is_signed;
    float factor, offset;
    uint16_t timeout_ms;
} roundGauge_can_entry_t;

typedef struct {
    uint8_t count;
    roundGauge_can_entry_t e[RG_CAN_MAP_MAX];
} roundGauge_can_map_t;

// Достать len бит (1..32) из data[8] по правилам DBC. false - поле выходит за кадр.
bool roundGauge_can_extract(const uint8_t data[8], uint8_t start, uint8_t len, roundGauge_can_order_t order,
                            bool is_signed, int64_t *raw);

// Читает таблицу из NVS; нет или битая - пустая. После nvs_flash_init().
void roundGauge_can_map_init(void);

// Копия текущей таблицы.
void roundGauge_can_map_copy(roundGauge_can_map_t *out);

// Разобрать JSON, проверить, сохранить в NVS и сделать текущей. ESP_ERR_INVALID_ARG -
// JSON не разобрался или в нём нет ни одной годной записи при непустом "map"
// (текущая таблица остаётся). Пустой "map" допустим - привязки сбрасываются.
esp_err_t roundGauge_can_map_apply_json(const char *json);

// JSON текущей таблицы в куче (освободить free()); NULL при нехватке памяти.
char *roundGauge_can_map_json_dup(void);

// Растёт при каждой смене таблицы: can_task по нему пересобирает свои структуры.
uint32_t roundGauge_can_map_generation(void);
