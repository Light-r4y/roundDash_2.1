#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "conf.h"

// Раскладка приборки: таблица сигналов и экраны. Хранится в NVS как JSON
// (RG_LAYOUT_NVS_KEY), в памяти - разобранной структурой. Картинки и шрифты лежат
// в разделе media, в layout только имена файлов (картинки - LVGL .bin, шрифты -
// LVGL bin font с расширением .fnt).
//
// {
//   "version": 2,
//   "signals": [
//     { "name": "rpm", "title": "RPM", "unit": "rpm", "min": 0, "max": 8000, "decimals": 0,
//       "zones": [ { "from": 6500, "to": 8000, "color": "#ff3030" } ] }
//   ],
//   "screens": [
//     { "type": "dial", "signal": "rpm",
//       "bg_color": "#000000", "bg_image": "",
//       "color": "#ffffff", "text_color": "#ffffff",
//       "angle": 270, "rotation": 135, "ticks": 41, "major_every": 5, "label_div": 1000,
//       "needle_color": "#ff8000", "needle_width": 6,
//       "tick_major_len": 22, "tick_minor_len": 10, "tick_major_width": 4, "tick_minor_width": 2,
//       "label_gap": 15, "label_font": "28", "needle_image": "", "needle_px": -1, "needle_py": -1,
//       "markers": [ { "value": 7000, "color": "#ff0000" } ],
//       "widgets": [
//         { "type": "value", "x": 0, "y": 60, "font": "48", "decimals": 2, "div": 1000 },
//         { "type": "text",  "x": 0, "y": -90, "text": "RPM", "font": "28", "color": "#9e9e9e" } ] }
//   ]
// }
//
// Экран = основной виджет ("dial" - циферблат, "ring" - заполнение по окружности,
// "number" - только число, "gmeter" - перегрузки; он рисует графику вокруг центра) + до
// RG_LAYOUT_MAX_WIDGETS дополнительных: value, text, image, indicator, bar, arc.
// Координаты x, y - пиксели от центра экрана, элемент центрируется в этой точке.
//
// Диапазон, единицы и знаки после точки берутся у сигнала; экран может задать свои
// "min"/"max" и свои "zones". Чего нет в JSON, берётся по умолчанию.
//
// Версия 1 (title/unit/show_value/decimals/zones прямо на экране, без таблицы
// сигналов) читается и превращается в версию 2 при разборе.

#define RG_LAYOUT_MAX_ZONES 4
#define RG_LAYOUT_MAX_MARKERS 4
#define RG_LAYOUT_MAX_LABELS 16
#define RG_LAYOUT_MAX_WIDGETS 4
#define RG_LAYOUT_MAX_TRAIL 12
#define RG_LAYOUT_MAX_SIGNALS 16 // = RG_SIGNAL_MAX_COUNT (common/signals)

typedef enum {
    RG_SCREEN_DIAL = 0,
    RG_SCREEN_RING,
    RG_SCREEN_NUMBER,
    RG_SCREEN_GMETER, // перегрузки: точка на круговой сетке, сигналы signal (вперёд-назад) и signal2 (влево-вправо)
} roundGauge_screen_type_t;

typedef enum {
    RG_WIDGET_VALUE = 0, // число сигнала
    RG_WIDGET_TEXT,      // статичная надпись
    RG_WIDGET_IMAGE,     // картинка из media
    RG_WIDGET_INDICATOR, // кружок или картинка, видны по условию
    RG_WIDGET_BAR,       // линейная полоса
    RG_WIDGET_ARC,       // мини-дуга
} roundGauge_widget_type_t;

typedef struct {
    float from, to;
    uint32_t color; // 0xRRGGBB
} roundGauge_zone_t;

typedef struct {
    float value;
    uint32_t color;
} roundGauge_marker_t;

typedef struct {
    char name[16];
    char title[24];
    char unit[12];
    float min, max;
    uint8_t decimals;
    uint8_t zone_count;
    roundGauge_zone_t zones[RG_LAYOUT_MAX_ZONES];
} roundGauge_signal_def_t;

typedef struct {
    roundGauge_widget_type_t type;
    int16_t x, y;
    uint16_t w, h;     // bar: размеры; arc и indicator: w - диаметр; image - размер из файла
    char signal[16];   // пусто - сигнал экрана
    char font[32];     // "14" | "28" | "48" | имя.fnt; пусто - по умолчанию для типа
    char text[24];     // text
    char image[32];    // image, indicator (вместо кружка)
    uint32_t color;    // текст, заливка, кружок
    uint32_t bg_color; // фон полосы и дуги
    int8_t decimals;   // value: -1 - как у сигнала
    float div;         // value: показываемое число = значение / div (по умолчанию 1); зоны - по исходному
    bool zone_color;   // value, bar, arc: цвет по зоне сигнала
    bool blink;        // indicator: мигать 2 Гц
    char op;           // indicator: '>' или '<'
    float threshold;   // indicator
    uint16_t angle;    // arc
    int16_t rotation;  // arc
    uint8_t width;     // arc: толщина
} roundGauge_widget_t;

typedef struct {
    roundGauge_screen_type_t type;
    char signal[16];
    char signal2[16];  // gmeter: второй сигнал (влево-вправо)
    bool has_range;    // min/max экрана вместо диапазона сигнала
    float min, max;
    uint32_t bg_color, color, text_color;
    char bg_image[32];
    // dial / ring
    uint16_t angle;
    int16_t rotation;
    // dial
    uint8_t ticks, major_every;
    float label_div;
    uint32_t needle_color;
    uint8_t needle_width;
    // Риски и подписи шкалы. Подпись стоит на радиусе: край - tick_major_len - label_gap.
    uint8_t tick_major_len, tick_minor_len;     // длина, px
    uint8_t tick_major_width, tick_minor_width; // толщина, px
    uint8_t label_gap;                          // от конца крупной риски до центра подписи, px
    char label_font[32];                        // "14" | "28" | "48" | файл .fnt из media
    char needle_image[32];
    // Ось вращения картинки-стрелки: точка картинки (px от левого верхнего угла), которая
    // ставится в центр шкалы. Меньше 0 - по оси "авто": x = 0, y = середина высоты.
    int16_t needle_px, needle_py;
    // gmeter: color - точка, text_color - сетка
    float g_range;     // шкала до внешнего кольца, g
    float g_step;      // шаг колец, g
    uint8_t trail;     // точек шлейфа, 0..RG_LAYOUT_MAX_TRAIL
    bool peaks;        // метки максимумов по четырём направлениям
    bool felt;         // точка показывает силу, которую чувствует водитель (торможение - вверх)
    // ring
    uint8_t ring_width;
    uint8_t zone_count, marker_count; // zone_count > 0 - зоны экрана вместо зон сигнала
    roundGauge_zone_t zones[RG_LAYOUT_MAX_ZONES];
    roundGauge_marker_t markers[RG_LAYOUT_MAX_MARKERS];
    uint8_t widget_count;
    roundGauge_widget_t widgets[RG_LAYOUT_MAX_WIDGETS];
} roundGauge_screen_t;

typedef struct {
    uint8_t signal_count;
    roundGauge_signal_def_t signals[RG_LAYOUT_MAX_SIGNALS];
    uint8_t count; // экранов
    roundGauge_screen_t screens[RG_UI_MAX_SCREENS];
} roundGauge_layout_t;

// Определение сигнала по имени или NULL.
const roundGauge_signal_def_t *roundGauge_layout_find_signal(const roundGauge_layout_t *l, const char *name);

// Читает layout из NVS, при отсутствии/ошибке берёт встроенный. После nvs_flash_init().
void roundGauge_layout_init(void);

// Копия текущей раскладки (экраны меняются на лету, держать указатель нельзя).
void roundGauge_layout_copy(roundGauge_layout_t *out);

// Разобрать JSON, проверить, сохранить в NVS и сделать текущим. ESP_ERR_INVALID_ARG -
// JSON не разобрался или в нём нет ни одного годного экрана (старый layout остаётся).
esp_err_t roundGauge_layout_apply_json(const char *json);

// JSON текущей раскладки в куче (освободить free()); NULL при нехватке памяти.
char *roundGauge_layout_json_dup(void);

// Вернуть встроенный layout и стереть сохранённый.
esp_err_t roundGauge_layout_reset(void);

// Версия счётчика: растёт при каждой смене layout, ui_task по ней пересобирает экраны.
uint32_t roundGauge_layout_generation(void);

// Заставить ui_task пересобрать экраны, не меняя layout (после смены картинок в media).
void roundGauge_layout_reload(void);
