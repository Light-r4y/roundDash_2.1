#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "alerts.h"

// Звук: зуммер платы. Режим (settings: sound.mode) - выключен, только тревоги по правилам (alerts.h) или
// тревоги и клики кнопок. Зуммер подключён к расширителю, тонов нет: писки и непрерывный сигнал.

void roundGauge_sound_task_start(void);

// Нажатие кнопки или свайп: заглушить текущие тревоги (до выхода значения из условия) и, в режиме
// "тревоги + клики", коротко пискнуть. Вызывать из любой задачи.
void roundGauge_sound_user_input(void);

// Пробный писк (RG_SOUND_TEST_MS) независимо от режима: проверить, что зуммер работает.
void roundGauge_sound_test(void);

typedef struct {
    uint8_t active;  // битовая маска правил, у которых тревога идёт сейчас
    uint8_t muted;   // битовая маска правил, заглушенных пользователем
    bool buzzer_on;  // зуммер включён в данный момент
} roundGauge_sound_status_t;

void roundGauge_sound_get_status(roundGauge_sound_status_t *out);
