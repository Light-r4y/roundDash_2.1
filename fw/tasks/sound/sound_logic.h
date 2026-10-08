#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "alerts.h"

// Логика тревог без железа (проверяется тестом на хосте): состояние одного правила и узор писков.

typedef struct {
    bool active;          // условие выполняется (с гистерезисом)
    bool muted;           // заглушена пользователем до выхода из условия
    uint32_t t_start_ms;  // время срабатывания
} roundGauge_alert_state_t;

// Обновить состояние правила. valid - есть ли у сигнала данные; v - значение. Тревога срабатывает,
// когда значение выходит за порог (> или <), снимается, когда вернулось за порог на hyst. Нет данных,
// правило выключено - тревоги нет. Выход из условия снимает и заглушку.
void roundGauge_alert_update(const roundGauge_alert_rule_t *r, roundGauge_alert_state_t *st, bool valid, float v,
                             uint32_t now_ms);

// Должен ли зуммер звучать в момент now_ms по этому правилу.
bool roundGauge_alert_output(const roundGauge_alert_rule_t *r, const roundGauge_alert_state_t *st, uint32_t now_ms);
