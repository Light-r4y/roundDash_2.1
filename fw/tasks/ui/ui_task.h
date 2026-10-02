#pragma once

// Отрисовка приборки (LVGL 9 через esp_lvgl_port).
//
// LVGL крутит собственная задача esp_lvgl_port (RG_LVGL_TASK_*): lv_timer_handler
// и рендер. ui_task раз в RG_UI_TASK_PERIOD_MS переносит значения сигналов из
// can_task в виджеты — только под lvgl_port_lock() — и переключает экраны по
// кнопкам. Картинки (фоны, стрелки) читаются из раздела media (RG_MEDIA_BASE_PATH).

// Создаёт задачу. Вызывать после roundGauge_board_init().
void roundGauge_ui_task_start(void);
