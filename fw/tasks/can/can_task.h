#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Приём CAN (TWAI, ESP-IDF v6: esp_twai_onchip).
//
// Путь кадра: колбэк on_rx_done из ISR -> очередь RG_CAN_RX_QUEUE_LEN -> can_task,
// которая достаёт значения сигналов по таблице привязок (common/config/can_map.h) и
// кладёт их в хранилище сигналов (common/signals). ISR только копирует кадр в очередь
// и заранее отбрасывает чужие ID: поиск по таблице и арифметика с float в
// прерывании не нужны. Пока веб читает сниффер, чужие ID не отбрасываются.
//
// Приём пассивный: приборка в шину ничего не передаёт. Режим (только прослушивание /
// с подтверждением кадров) и скорость - из roundGauge_settings_get_can(); их смену и
// смену таблицы привязок can_task подхватывает на лету, без перезагрузки.
//
// Если в настройках включено "демо" (can.demo), значения сигналов вместо шины гонит
// генератор (common/signals).

// Создаёт очередь и задачу. Вызывать после roundGauge_settings_init() и
// roundGauge_can_map_init().
void roundGauge_can_task_start(void);

// Применить текущие настройки CAN (скорость, режим, демо) на лету: узел TWAI
// пересоздаётся. Из любой задачи, не блокирует - выполнит can_task.
void roundGauge_can_apply_settings(void);

// Состояние шины и счётчики для веба.
typedef struct {
    bool node_up;          // узел TWAI создан и включён
    uint8_t state;         // 0 активен, 1 предупреждение, 2 пассивный, 3 bus-off
    uint32_t rx;           // кадров на шине принято (до фильтра)
    uint32_t dropped;      // потеряно из-за переполнения очереди
    uint16_t tx_err, rx_err;
    uint32_t bus_err;      // ошибок шины с момента включения узла
    uint32_t busoff_count; // сколько раз был bus-off
} roundGauge_can_status_t;

void roundGauge_can_get_status(roundGauge_can_status_t *out);

// Строка сниффера: последний кадр данного ID.
typedef struct {
    uint32_t id;
    bool ext;
    uint8_t dlc;
    uint8_t data[8];
    uint32_t count;   // кадров с этим ID
    uint32_t age_ms;  // сколько прошло с последнего
} roundGauge_can_frame_info_t;

// Снимок сниффера (до RG_CAN_SNIFFER_MAX ID). Каждый вызов продлевает приём чужих ID
// на RG_CAN_SNIFFER_ACTIVE_MS. Возвращает число строк.
size_t roundGauge_can_sniffer_snapshot(roundGauge_can_frame_info_t *out, size_t max);
