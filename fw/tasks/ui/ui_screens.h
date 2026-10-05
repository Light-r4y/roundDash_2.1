#pragma once
#include "layout.h"

// Сборка и обновление экранов приборки по layout. Всё - только под lvgl_port_lock()
// (или из колбэка LVGL).

// Собрать экраны заново по layout, показать тот же по номеру (или последний, если
// экранов стало меньше), старые удалить.
void ui_screens_rebuild(const roundGauge_layout_t *layout);

// Перенести значения сигналов в виджеты текущего экрана (меняется только то, что
// изменилось).
void ui_screens_update(void);

// Переключить экран: +1 следующий, -1 предыдущий, по кругу.
void ui_screens_switch(int dir);

int ui_screens_current(void);
int ui_screens_count(void);
