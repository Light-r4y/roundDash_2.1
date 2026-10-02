#pragma once

// Опрос двух кнопок на разъёме J9 раз в RG_BUTTONS_TICK_MS.
//
// Удержание кнопки 1 дольше RG_BTN_AP_HOLD_MS поднимает точку доступа
// (roundGauge_webcfg_start()). Остальные действия кнопок ещё не определены.

void roundGauge_buttons_task_start(void);
