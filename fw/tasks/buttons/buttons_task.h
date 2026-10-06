#pragma once

// Опрос двух кнопок на разъёме J9 раз в RG_BUTTONS_TICK_MS.
//
// Кнопка 1: удержание дольше RG_BTN_AP_HOLD_MS поднимает точку доступа
// (roundGauge_webcfg_start()), дольше RG_AUTH_RESET_HOLD_MS - ещё и снимает пароль на
// настройки; короткое нажатие - следующий экран.
// Кнопка 2: нажатие - яркость по кругу (RG_BTN2_BRIGHTNESS_STEPS). Остальные действия ещё не определены.

void roundGauge_buttons_task_start(void);
