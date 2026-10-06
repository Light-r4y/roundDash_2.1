#include "ui_screens.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "conf.h"
#include "signals.h"
#include "lvgl.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "src/image/lv_image_decoder_private.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "UI";

// Внутреннее разрешение шкалы, дуги и полосы: значения сигнала приводятся к 0..RES,
// чтобы стрелка двигалась плавно при любых min/max (виджеты LVGL целочисленные).
#define RES 1000
#define NEEDLE_MARGIN 55
#define BLINK_HALF_PERIOD_MS 250 // мигание 2 Гц
#define MAX_FONTS (RG_UI_MAX_SCREENS * (RG_LAYOUT_MAX_WIDGETS + 1)) // виджеты и подписи шкалы

// Сигнал с параметрами, разобранными по таблице сигналов layout.
typedef struct {
    int id;
    float min, max;
    uint8_t decimals;
    const roundGauge_zone_t *zones;
    uint8_t zone_count;
} sig_t;

typedef struct {
    lv_obj_t *obj;
    const roundGauge_widget_t *cfg;
    sig_t sig;
    bool shown;
    int32_t key;  // что показано: позиция, текст или видимость
    int zone;     // текущая зона (для цвета)
} ui_widget_t;

typedef struct {
    lv_obj_t *scr;
    const roundGauge_screen_t *cfg; // указывает внутрь ui_set_t.layout
    sig_t sig;                      // сигнал основного виджета (с переопределениями экрана)
    lv_obj_t *scale, *needle, *arc;
    bool needle_is_image;
    lv_point_precise_t needle_pts[2];  // линия-стрелка держит указатель на эти точки
    int cur_zone;
    // Что показано сейчас - чтобы не трогать виджет без изменений.
    bool shown;
    int32_t pos_key;
    char labels[RG_LAYOUT_MAX_LABELS][12];
    const char *label_ptrs[RG_LAYOUT_MAX_LABELS + 1];
    lv_style_t zone_style[RG_LAYOUT_MAX_ZONES];
    lv_point_precise_t marker_pts[RG_LAYOUT_MAX_MARKERS][2]; // линии держат указатель на точки
    ui_widget_t w[RG_LAYOUT_MAX_WIDGETS];
    int w_count;
    // Д-метр (gmeter): точка, шлейф и метки максимумов.
    sig_t sig2;
    lv_obj_t *gm_dot, *gm_trail[RG_LAYOUT_MAX_TRAIL], *gm_peak[4], *gm_peak_lbl[4];
    int gm_hx[RG_LAYOUT_MAX_TRAIL + 1], gm_hy[RG_LAYOUT_MAX_TRAIL + 1]; // история точки: [0] - текущая
    int gm_hn;
    int gm_dx, gm_dy;
    bool gm_visible;
    int64_t gm_trail_ms, gm_last_ms;
    float gm_peak_v[4];          // вверх, вниз, влево, вправо - в g
    int64_t gm_peak_ms[4];
    int gm_peak_txt[4];
} ui_screen_t;

typedef struct {
    char name[32];
    lv_font_t *font;
} font_slot_t;

typedef struct {
    roundGauge_layout_t layout;
    ui_screen_t s[RG_UI_MAX_SCREENS];
    int count, cur;
    font_slot_t fonts[MAX_FONTS]; // загруженные из media шрифты - общие для всех виджетов
    int font_count;
} ui_set_t;

static ui_set_t *s_set;

// ------------------------------------------------------------------
// Вспомогательное
// ------------------------------------------------------------------

static int32_t to_res(float min, float max, float v)
{
    float f = (v - min) / (max - min);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    return (int32_t)lroundf(f * RES);
}

static bool media_file_exists(const char *name)
{
    char path[sizeof(RG_MEDIA_BASE_PATH) + 40];
    snprintf(path, sizeof(path), RG_MEDIA_BASE_PATH "/%s", name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    fclose(f);
    return true;
}

// "M:/name" - буква диска и корень заданы в sdkconfig.defaults (LV_FS_POSIX_*).
static void media_src(const char *name, char *out, size_t size)
{
    snprintf(out, size, "M:/%s", name);
}

// Шрифт по имени из layout: "14" | "28" | "48" - встроенные; иначе файл из media
// (LVGL bin font, целиком читается в RAM при создании). Нет файла - шрифт 28.
static const lv_font_t *font_get(ui_set_t *set, const char *name)
{
    if (name[0] == '\0' || strcmp(name, "28") == 0) return &lv_font_montserrat_28;
    if (strcmp(name, "14") == 0) return &lv_font_montserrat_14;
    if (strcmp(name, "48") == 0) return &lv_font_montserrat_48;

    for (int i = 0; i < set->font_count; i++) {
        if (strcmp(set->fonts[i].name, name) == 0) {
            return set->fonts[i].font ? set->fonts[i].font : &lv_font_montserrat_28;
        }
    }
    if (set->font_count >= MAX_FONTS) {
        return &lv_font_montserrat_28;
    }
    font_slot_t *slot = &set->fonts[set->font_count++];
    strlcpy(slot->name, name, sizeof(slot->name));
    slot->font = NULL;
    if (media_file_exists(name)) {
        char src[48];
        media_src(name, src, sizeof(src));
        slot->font = lv_binfont_create(src);
    }
    if (slot->font == NULL) {
        ESP_LOGW(TAG, "font \"%s\" not loaded, using 28", name);
        return &lv_font_montserrat_28;
    }
    return slot->font;
}

// Параметры сигнала по таблице layout. Нет в таблице - диапазон 0..100, без зон.
static void resolve_signal(const roundGauge_layout_t *l, const char *name, sig_t *out)
{
    const roundGauge_signal_def_t *d = roundGauge_layout_find_signal(l, name);
    memset(out, 0, sizeof(*out));
    out->min = 0;
    out->max = 100;
    if (d != NULL) {
        out->min = d->min;
        out->max = d->max;
        out->decimals = d->decimals;
        out->zones = d->zones;
        out->zone_count = d->zone_count;
    } else {
        ESP_LOGW(TAG, "signal \"%s\" is not in the signal table", name);
    }
    out->id = roundGauge_signal_register(name, out->min, out->max);
}

static int zone_index(const sig_t *s, float v)
{
    for (int i = 0; i < s->zone_count; i++) {
        if (v >= s->zones[i].from && v <= s->zones[i].to) {
            return i;
        }
    }
    return -1;
}

static void not_clickable(lv_obj_t *o)
{
    lv_obj_set_clickable(o, false);
    lv_obj_set_scrollable(o, false);
}

// ------------------------------------------------------------------
// Основной виджет: графика вокруг центра
// ------------------------------------------------------------------

static void build_zones_on_scale(ui_screen_t *u)
{
    const roundGauge_screen_t *c = u->cfg;
    for (int i = 0; i < u->sig.zone_count; i++) {
        lv_style_t *st = &u->zone_style[i];
        lv_color_t col = lv_color_hex(u->sig.zones[i].color);
        lv_style_init(st);
        lv_style_set_arc_color(st, col);
        lv_style_set_arc_width(st, 8);
        lv_style_set_line_color(st, col);
        lv_style_set_text_color(st, col);
        lv_scale_section_t *sec = lv_scale_add_section(u->scale);
        lv_scale_set_section_range(u->scale, sec, to_res(u->sig.min, u->sig.max, u->sig.zones[i].from),
                                   to_res(u->sig.min, u->sig.max, u->sig.zones[i].to));
        lv_scale_set_section_style_main(u->scale, sec, st);
        lv_scale_set_section_style_indicator(u->scale, sec, st);
        lv_scale_set_section_style_items(u->scale, sec, st);
    }
    (void)c;
}

static void build_dial(ui_set_t *set, ui_screen_t *u)
{
    const roundGauge_screen_t *c = u->cfg;
    const int size = RG_UI_DIAL_SIZE;

    lv_obj_t *scale = lv_scale_create(u->scr);
    u->scale = scale;
    lv_obj_set_size(scale, size, size);
    lv_obj_center(scale);
    not_clickable(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(scale, 0, RES);
    lv_scale_set_total_tick_count(scale, c->ticks);
    lv_scale_set_major_tick_every(scale, c->major_every);
    lv_scale_set_label_show(scale, true);
    lv_scale_set_angle_range(scale, c->angle);
    lv_scale_set_rotation(scale, c->rotation);

    // Подписи крупных делений: значения сигнала, делённые на label_div.
    int nlab = (c->ticks - 1) / c->major_every + 1;
    for (int i = 0; i < nlab; i++) {
        float frac = nlab > 1 ? (float)(i * c->major_every) / (float)(c->ticks - 1) : 0;
        float x = (u->sig.min + (u->sig.max - u->sig.min) * frac) / c->label_div;
        if (fabsf(x - roundf(x)) < 0.05f) {
            snprintf(u->labels[i], sizeof(u->labels[i]), "%.0f", x);
        } else {
            snprintf(u->labels[i], sizeof(u->labels[i]), "%.1f", x);
        }
        u->label_ptrs[i] = u->labels[i];
    }
    u->label_ptrs[nlab] = NULL;
    lv_scale_set_text_src(scale, u->label_ptrs);

    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(scale, font_get(set, c->label_font), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_hex(c->text_color), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, c->tick_major_len, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, c->tick_minor_len, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, c->tick_major_width, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, c->tick_minor_width, LV_PART_ITEMS);
    // LVGL ставит подпись на радиус: край - длина крупной риски - (15 + pad_radial).
    lv_obj_set_style_pad_radial(scale, (int32_t)c->label_gap - 15, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_color_hex(c->color), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, lv_palette_main(LV_PALETTE_GREY), LV_PART_ITEMS);
    lv_obj_set_style_arc_width(scale, 0, LV_PART_MAIN);

    build_zones_on_scale(u);

    // Метки на шкале: короткая черта поперёк делений, от радиуса 180 до 218 px.
    for (int i = 0; i < c->marker_count; i++) {
        float ang = (float)(c->rotation + c->angle * to_res(u->sig.min, u->sig.max, c->markers[i].value) / (float)RES) *
                    (float)M_PI / 180.0f;
        const float cx = size / 2.0f, r1 = size / 2.0f - 40, r2 = size / 2.0f - 2;
        u->marker_pts[i][0] = (lv_point_precise_t){cx + r1 * cosf(ang), cx + r1 * sinf(ang)};
        u->marker_pts[i][1] = (lv_point_precise_t){cx + r2 * cosf(ang), cx + r2 * sinf(ang)};
        lv_obj_t *m = lv_line_create(scale);
        lv_obj_set_style_line_width(m, 4, 0);
        lv_obj_set_style_line_color(m, lv_color_hex(c->markers[i].color), 0);
        lv_line_set_points(m, u->marker_pts[i], 2);
    }

    // Стрелка: картинка из media (смотрит вправо; ось вращения задана needle_px/needle_py,
    // по умолчанию слева по центру) либо, если картинки нет, линия.
    if (c->needle_image[0] && media_file_exists(c->needle_image)) {
        char src[48];
        media_src(c->needle_image, src, sizeof(src));
        lv_obj_t *img = lv_image_create(scale);
        lv_image_set_src(img, src);
        lv_obj_update_layout(img);
        int w = lv_obj_get_width(img), h = lv_obj_get_height(img);
        int px = c->needle_px >= 0 ? LV_MIN(c->needle_px, w) : 0;
        int py = c->needle_py >= 0 ? LV_MIN(c->needle_py, h) : h / 2;
        lv_image_set_pivot(img, px, py);
        lv_obj_set_pos(img, size / 2 - px, size / 2 - py);
        u->needle = img;
        u->needle_is_image = true;
    } else {
        if (c->needle_image[0]) {
            ESP_LOGW(TAG, "needle image \"%s\" not found, drawing a line", c->needle_image);
        }
        lv_obj_t *line = lv_line_create(scale);
        lv_obj_set_style_line_width(line, c->needle_width, 0);
        lv_obj_set_style_line_rounded(line, true, 0);
        lv_obj_set_style_line_color(line, lv_color_hex(c->needle_color), 0);
        u->needle = line;
    }
}

static lv_obj_t *make_arc(lv_obj_t *parent, int size, int width, int angle, int rotation, uint32_t bg, uint32_t fg)
{
    lv_obj_t *arc = lv_arc_create(parent);
    lv_obj_set_size(arc, size, size);
    not_clickable(arc);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_arc_set_rotation(arc, rotation);
    lv_arc_set_bg_angles(arc, 0, angle);
    lv_arc_set_range(arc, 0, RES);
    lv_arc_set_value(arc, 0);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, lv_color_hex(fg), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, false, LV_PART_INDICATOR);
    return arc;
}

static void build_ring(ui_screen_t *u)
{
    const roundGauge_screen_t *c = u->cfg;
    u->arc = make_arc(u->scr, RG_UI_DIAL_SIZE, c->ring_width, c->angle, c->rotation, 0x303030, c->color);
    lv_obj_center(u->arc);
    u->cur_zone = -1;
}

// ------------------------------------------------------------------
// Д-метр: перегрузки точкой на круговой сетке
// ------------------------------------------------------------------
#define GM_RADIUS 180          // радиус внешнего кольца, px
#define GM_DOT_D 28
#define GM_PEAK_D 10
#define GM_TRAIL_STEP_MS 50    // шаг записи шлейфа
#define GM_PEAK_HOLD_MS 8000   // сколько максимум держится, потом спадает
#define GM_PEAK_DECAY_G_S 0.25f
#define GM_DOT_CLAMP 1.05f     // точка не уходит дальше внешнего кольца больше чем на 5 %

static lv_obj_t *gm_circle(lv_obj_t *parent, int d, lv_color_t fill, lv_opa_t fill_opa, lv_color_t border, int border_w)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, fill, 0);
    lv_obj_set_style_bg_opa(o, fill_opa, 0);
    lv_obj_set_style_border_color(o, border, 0);
    lv_obj_set_style_border_width(o, border_w, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    not_clickable(o);
    return o;
}

static lv_obj_t *gm_bar(lv_obj_t *parent, int w, int h, lv_color_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    not_clickable(o);
    lv_obj_align(o, LV_ALIGN_CENTER, 0, 0);
    return o;
}

static void build_gmeter(ui_screen_t *u)
{
    const roundGauge_screen_t *c = u->cfg;
    const float scale = GM_RADIUS / c->g_range;
    lv_color_t grid = lv_color_hex(c->text_color);

    // Кольца через g_step; внешнее толще.
    for (int k = 1; k * c->g_step <= c->g_range + 0.001f; k++) {
        int d = (int)lroundf(2 * k * c->g_step * scale);
        bool outer = k * c->g_step >= c->g_range - 0.001f;
        lv_obj_t *ring = gm_circle(u->scr, d, grid, LV_OPA_TRANSP, grid, outer ? 3 : 2);
        lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
    }
    gm_bar(u->scr, 2, 2 * GM_RADIUS, grid);
    gm_bar(u->scr, 2 * GM_RADIUS, 2, grid);

    // Метки максимумов: маркер на оси и число за внешним кольцом.
    if (c->peaks) {
        const int lx[4] = { 0, 0, -(GM_RADIUS + 36), GM_RADIUS + 36 };
        const int ly[4] = { -(GM_RADIUS + 24), GM_RADIUS + 24, 0, 0 };
        for (int i = 0; i < 4; i++) {
            u->gm_peak[i] = gm_circle(u->scr, GM_PEAK_D, lv_color_hex(0xFF4040), LV_OPA_COVER, lv_color_hex(0xFF4040), 0);
            lv_obj_set_hidden(u->gm_peak[i], true);
            u->gm_peak_lbl[i] = lv_label_create(u->scr);
            lv_obj_set_style_text_font(u->gm_peak_lbl[i], &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(u->gm_peak_lbl[i], lv_color_hex(0xFF8080), 0);
            lv_obj_align(u->gm_peak_lbl[i], LV_ALIGN_CENTER, lx[i], ly[i]);
            lv_obj_set_hidden(u->gm_peak_lbl[i], true);
            u->gm_peak_txt[i] = -1;
        }
    }

    // Шлейф: от самой старой (маленькой и прозрачной) к новой; новые рисуются выше.
    for (int k = c->trail - 1; k >= 0; k--) {
        int d = 8 + (GM_DOT_D - 12) * (c->trail - k) / (c->trail + 1);
        lv_opa_t opa = (lv_opa_t)(180 - 150 * k / (c->trail > 1 ? c->trail - 1 : 1));
        u->gm_trail[k] = gm_circle(u->scr, d, lv_color_hex(c->color), opa, lv_color_hex(c->color), 0);
        lv_obj_set_hidden(u->gm_trail[k], true);
    }

    u->gm_dot = gm_circle(u->scr, GM_DOT_D, lv_color_hex(c->color), LV_OPA_COVER, lv_color_white(), 2);
    lv_obj_align(u->gm_dot, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_hidden(u->gm_dot, true);
}

// Скрыть точку, шлейф и максимумы (нет данных датчика).
static void gmeter_hide(ui_screen_t *u)
{
    lv_obj_set_hidden(u->gm_dot, true);
    for (int k = 0; k < u->cfg->trail; k++) lv_obj_set_hidden(u->gm_trail[k], true);
    for (int i = 0; i < 4; i++) {
        if (u->gm_peak[i]) {
            lv_obj_set_hidden(u->gm_peak[i], true);
            lv_obj_set_hidden(u->gm_peak_lbl[i], true);
        }
    }
    u->gm_visible = false;
    u->gm_hn = 0;
}

static void update_gmeter(ui_screen_t *u)
{
    const roundGauge_screen_t *c = u->cfg;
    float lon, lat;
    if (!(roundGauge_signal_get(u->sig.id, &lon) && roundGauge_signal_get(u->sig2.id, &lat))) {
        if (u->gm_visible || !u->shown) {
            gmeter_hide(u);
        }
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    const float scale = GM_RADIUS / c->g_range;

    // Экранные оси: вправо +, вверх +. Точка показывает силу, которую чувствует водитель
    // (при торможении вверх, при правом повороте влево), если не отключено "felt".
    float sx = c->felt ? -lat : lat;
    float sy = c->felt ? -lon : lon;
    float m = hypotf(sx, sy), lim = c->g_range * GM_DOT_CLAMP;
    if (m > lim) {
        sx *= lim / m;
        sy *= lim / m;
    }
    int px = (int)lroundf(sx * scale), py = -(int)lroundf(sy * scale);

    if (!u->gm_visible || px != u->gm_dx || py != u->gm_dy) {
        lv_obj_align(u->gm_dot, LV_ALIGN_CENTER, px, py);
        u->gm_dx = px;
        u->gm_dy = py;
    }
    if (!u->gm_visible) {
        lv_obj_set_hidden(u->gm_dot, false);
        u->gm_visible = true;
        u->gm_trail_ms = now;
        u->gm_last_ms = now;
        u->gm_hn = 0;
        for (int i = 0; i < 4; i++) {
            u->gm_peak_v[i] = 0;
            u->gm_peak_ms[i] = now;
        }
    }

    // Шлейф: каждые GM_TRAIL_STEP_MS записываем положение и сдвигаем точки по истории.
    if (c->trail > 0 && now - u->gm_trail_ms >= GM_TRAIL_STEP_MS) {
        u->gm_trail_ms = now;
        int n = c->trail;
        for (int i = n; i > 0; i--) {
            u->gm_hx[i] = u->gm_hx[i - 1];
            u->gm_hy[i] = u->gm_hy[i - 1];
        }
        u->gm_hx[0] = px;
        u->gm_hy[0] = py;
        if (u->gm_hn < n + 1) u->gm_hn++;
        for (int k = 0; k < n; k++) {
            if (k + 1 < u->gm_hn) {
                lv_obj_align(u->gm_trail[k], LV_ALIGN_CENTER, u->gm_hx[k + 1], u->gm_hy[k + 1]);
                lv_obj_set_hidden(u->gm_trail[k], false);
            }
        }
    }

    // Максимумы по направлениям экрана: вверх, вниз, влево, вправо. Держатся, потом спадают.
    if (c->peaks) {
        float dt = (float)(now - u->gm_last_ms) / 1000.0f;
        if (dt > 0.2f) dt = 0.2f;
        const float cur[4] = { fmaxf(sy, 0), fmaxf(-sy, 0), fmaxf(-sx, 0), fmaxf(sx, 0) };
        for (int i = 0; i < 4; i++) {
            if (cur[i] >= u->gm_peak_v[i]) {
                u->gm_peak_v[i] = cur[i];
                u->gm_peak_ms[i] = now;
            } else if (now - u->gm_peak_ms[i] > GM_PEAK_HOLD_MS) {
                u->gm_peak_v[i] = fmaxf(cur[i], u->gm_peak_v[i] - GM_PEAK_DECAY_G_S * dt);
            }
            float v = u->gm_peak_v[i];
            bool show = v > 0.05f;
            lv_obj_set_hidden(u->gm_peak[i], !show);
            lv_obj_set_hidden(u->gm_peak_lbl[i], !show);
            if (!show) {
                u->gm_peak_txt[i] = -1;
                continue;
            }
            int dist = (int)lroundf(v * scale);
            const int ax[4] = { 0, 0, -1, 1 }, ay[4] = { -1, 1, 0, 0 };
            lv_obj_align(u->gm_peak[i], LV_ALIGN_CENTER, ax[i] * dist, ay[i] * dist);
            int txt = (int)lroundf(v * 100);
            if (txt != u->gm_peak_txt[i]) {
                char s[12];
                snprintf(s, sizeof(s), "%.2f", (double)v); // встроенный sprintf LVGL не умеет %f
                lv_label_set_text(u->gm_peak_lbl[i], s);
                u->gm_peak_txt[i] = txt;
            }
        }
    }
    u->gm_last_ms = now;
}

// ------------------------------------------------------------------
// Дополнительные виджеты
// ------------------------------------------------------------------

static void build_widget(ui_set_t *set, ui_screen_t *u, const roundGauge_widget_t *c)
{
    ui_widget_t *w = &u->w[u->w_count];
    memset(w, 0, sizeof(*w));
    w->cfg = c;
    w->zone = -2; // не определена
    resolve_signal(&set->layout, c->signal[0] ? c->signal : u->cfg->signal, &w->sig);
    if (c->decimals >= 0) {
        w->sig.decimals = (uint8_t)c->decimals;
    }

    lv_obj_t *o = NULL;
    switch (c->type) {
    case RG_WIDGET_VALUE:
    case RG_WIDGET_TEXT:
        o = lv_label_create(u->scr);
        lv_label_set_text(o, c->type == RG_WIDGET_TEXT ? c->text : "--");
        lv_obj_set_style_text_font(o, font_get(set, c->font), 0);
        lv_obj_set_style_text_color(o, lv_color_hex(c->color), 0);
        break;
    case RG_WIDGET_IMAGE:
        if (c->image[0] && media_file_exists(c->image)) {
            char src[48];
            media_src(c->image, src, sizeof(src));
            o = lv_image_create(u->scr);
            lv_image_set_src(o, src);
        } else if (c->image[0]) {
            ESP_LOGW(TAG, "image \"%s\" not found", c->image);
        }
        break;
    case RG_WIDGET_INDICATOR:
        if (c->image[0] && media_file_exists(c->image)) {
            char src[48];
            media_src(c->image, src, sizeof(src));
            o = lv_image_create(u->scr);
            lv_image_set_src(o, src);
        } else {
            o = lv_obj_create(u->scr);
            lv_obj_set_size(o, c->w, c->w);
            lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(o, lv_color_hex(c->color), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(o, 0, 0);
            lv_obj_set_style_pad_all(o, 0, 0);
        }
        lv_obj_set_hidden(o, true); // покажет update по условию
        break;
    case RG_WIDGET_BAR:
        o = lv_bar_create(u->scr);
        lv_obj_set_size(o, c->w, c->h);
        lv_bar_set_range(o, 0, RES);
        lv_bar_set_value(o, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(o, lv_color_hex(c->bg_color), LV_PART_MAIN);
        lv_obj_set_style_bg_color(o, lv_color_hex(c->color), LV_PART_INDICATOR);
        lv_obj_set_style_radius(o, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(o, 3, LV_PART_INDICATOR);
        break;
    case RG_WIDGET_ARC:
        o = make_arc(u->scr, c->w, c->width, c->angle, c->rotation, c->bg_color, c->color);
        break;
    }
    if (o == NULL) {
        return; // картинки нет - виджет пропускаем
    }
    not_clickable(o);
    lv_obj_align(o, LV_ALIGN_CENTER, c->x, c->y);
    w->obj = o;
    u->w_count++;
}

// ------------------------------------------------------------------
// Экран
// ------------------------------------------------------------------

static void build_screen(ui_set_t *set, ui_screen_t *u, const roundGauge_screen_t *c)
{
    memset(u, 0, sizeof(*u));
    u->cfg = c;

    // Основной сигнал: параметры из таблицы, диапазон и зоны экрана - поверх.
    resolve_signal(&set->layout, c->signal, &u->sig);
    if (c->has_range) {
        u->sig.min = c->min;
        u->sig.max = c->max;
    }
    if (c->zone_count > 0) {
        u->sig.zones = c->zones;
        u->sig.zone_count = c->zone_count;
    }

    u->scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(u->scr, lv_color_hex(c->bg_color), 0);
    lv_obj_set_style_bg_opa(u->scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(u->scr, lv_color_hex(c->text_color), 0);
    lv_obj_set_style_border_width(u->scr, 0, 0);
    lv_obj_set_style_pad_all(u->scr, 0, 0);
    lv_obj_set_scrollable(u->scr, false);

    if (c->bg_image[0]) {
        if (media_file_exists(c->bg_image)) {
            char src[48];
            media_src(c->bg_image, src, sizeof(src));
            lv_obj_t *bg = lv_image_create(u->scr);
            lv_image_set_src(bg, src);
            lv_obj_align(bg, LV_ALIGN_CENTER, 0, 0);
            not_clickable(bg);
        } else {
            ESP_LOGW(TAG, "background \"%s\" not found", c->bg_image);
        }
    }

    switch (c->type) {
    case RG_SCREEN_DIAL:
        build_dial(set, u);
        break;
    case RG_SCREEN_RING:
        build_ring(u);
        break;
    case RG_SCREEN_NUMBER:
        break;
    case RG_SCREEN_GMETER:
        resolve_signal(&set->layout, c->signal2, &u->sig2);
        build_gmeter(u);
        break;
    }

    for (int i = 0; i < c->widget_count; i++) {
        build_widget(set, u, &c->widgets[i]);
    }
}

static void destroy_set(ui_set_t *set)
{
    for (int i = 0; i < set->count; i++) {
        lv_obj_delete(set->s[i].scr);
        for (int z = 0; z < set->s[i].sig.zone_count; z++) {
            lv_style_reset(&set->s[i].zone_style[z]);
        }
    }
    // Шрифты - после объектов, которые ими рисовали.
    for (int i = 0; i < set->font_count; i++) {
        if (set->fonts[i].font != NULL) {
            lv_binfont_destroy(set->fonts[i].font);
        }
    }
    free(set);
}

// Прогрев кэша картинок. LVGL читает .bin из flash в RAM при первой отрисовке, то есть при первом показе
// экрана (фон 480x480 - 450 КБ, порядка сотен миллисекунд): отсюда просадка FPS при первом переключении.
// Здесь открываем и сразу закрываем каждую картинку раскладки: запись остаётся в кэше (ключ - путь и тип
// источника, параметры открытия в него не входят), и первый показ экрана идёт без загрузки с flash.
static void preload_images(const roundGauge_layout_t *l)
{
    const char *seen[RG_UI_MAX_SCREENS * (2 + RG_LAYOUT_MAX_WIDGETS)];
    int n_seen = 0;
    uint32_t bytes = 0;
    int loaded = 0;
    int64_t t_all = esp_timer_get_time();

    for (size_t i = 0; i < l->count; i++) {
        const roundGauge_screen_t *s = &l->screens[i];
        const char *names[2 + RG_LAYOUT_MAX_WIDGETS];
        int n = 0;
        names[n++] = s->bg_image;
        if (s->type == RG_SCREEN_DIAL) {
            names[n++] = s->needle_image;
        }
        for (size_t k = 0; k < s->widget_count; k++) {
            names[n++] = s->widgets[k].image;
        }
        for (int j = 0; j < n; j++) {
            const char *name = names[j];
            if (name[0] == '\0' || !media_file_exists(name)) {
                continue;
            }
            bool dup = false;
            for (int q = 0; q < n_seen && !dup; q++) {
                dup = strcmp(seen[q], name) == 0;
            }
            if (dup || n_seen >= (int)(sizeof(seen) / sizeof(seen[0]))) {
                continue;
            }
            seen[n_seen++] = name;

            char src[48];
            media_src(name, src, sizeof(src));
            int64_t t0 = esp_timer_get_time();
            lv_image_decoder_dsc_t dsc;
            if (lv_image_decoder_open(&dsc, src, NULL) == LV_RESULT_OK) {
                uint32_t sz = dsc.decoded != NULL ? dsc.decoded->data_size : 0;
                lv_image_decoder_close(&dsc);
                bytes += sz;
                loaded++;
                ESP_LOGI(TAG, "Preloaded %s: %u KB, %d ms", name, (unsigned)(sz / 1024),
                         (int)((esp_timer_get_time() - t0) / 1000));
            } else {
                ESP_LOGW(TAG, "Preload of %s failed", name);
            }
        }
    }
    ESP_LOGI(TAG, "Image preload: %d files, %u KB decoded, cache %u KB, %d ms", loaded, (unsigned)(bytes / 1024),
             (unsigned)(LV_CACHE_DEF_SIZE / 1024), (int)((esp_timer_get_time() - t_all) / 1000));
    if (bytes > LV_CACHE_DEF_SIZE) {
        ESP_LOGW(TAG, "Images do not fit the cache: some will be reloaded from flash on screen switches");
    }
}

void ui_screens_rebuild(const roundGauge_layout_t *layout)
{
    // Картинки могли смениться под тем же именем - кэш декодера устарел.
    lv_image_cache_drop(NULL);
    lv_image_header_cache_drop(NULL);

    ui_set_t *ns = calloc(1, sizeof(*ns));
    if (ns == NULL) {
        ESP_LOGE(TAG, "no memory for screens");
        return;
    }
    ns->layout = *layout;
    ns->count = layout->count;

    // Все сигналы таблицы - в хранилище: их диапазоны нужны генератору, а id -
    // тем, кто пишет значения.
    for (int i = 0; i < ns->layout.signal_count; i++) {
        roundGauge_signal_register(ns->layout.signals[i].name, ns->layout.signals[i].min, ns->layout.signals[i].max);
    }
    for (int i = 0; i < ns->count; i++) {
        build_screen(ns, &ns->s[i], &ns->layout.screens[i]);
    }
    ns->cur = 0;
    if (s_set != NULL) {
        ns->cur = s_set->cur < ns->count ? s_set->cur : ns->count - 1;
    }
    preload_images(&ns->layout);
    lv_screen_load(ns->s[ns->cur].scr);

    ui_set_t *old = s_set;
    s_set = ns;
    if (old != NULL) {
        destroy_set(old);
    }
    ESP_LOGI(TAG, "%d screens built, showing #%d", ns->count, ns->cur + 1);
}

int ui_screens_current(void)
{
    return s_set ? s_set->cur : 0;
}

int ui_screens_count(void)
{
    return s_set ? s_set->count : 0;
}

void ui_screens_switch(int dir)
{
    if (s_set == NULL || s_set->count < 2) {
        return;
    }
    s_set->cur = (s_set->cur + dir + s_set->count) % s_set->count;
    ui_screen_t *u = &s_set->s[s_set->cur];
    u->shown = false; // значения могли уйти вперёд, пока экран не был виден
    u->gm_visible = false;
    for (int i = 0; i < u->w_count; i++) {
        u->w[i].shown = false;
    }
    lv_screen_load(u->scr);
}

// ------------------------------------------------------------------
// Обновление значений
// ------------------------------------------------------------------

// Стрелка-линия. lv_scale_set_line_needle_value здесь не годится: точки у него
// считаются от угла шкалы, а размер lv_line равен максимальным координатам точек -
// объект получается от левого верхнего угла шкалы до кончика (до ~150 тыс. пикселей),
// и LVGL перерисовывает всё это при каждом шаге стрелки. Поэтому точки считаем
// сами, от левого верхнего угла охватывающего прямоугольника отрезка, а сам объект
// ставим в этот угол: перерисовывается только прямоугольник вокруг стрелки.
static void needle_line_set(ui_screen_t *u, int32_t pos)
{
    const roundGauge_screen_t *c = u->cfg;
    const float centre = RG_UI_DIAL_SIZE / 2.0f;
    const float len = RG_UI_DIAL_SIZE / 2.0f - NEEDLE_MARGIN;
    float ang = (float)(c->rotation + c->angle * pos / (float)RES) * (float)M_PI / 180.0f;
    float tx = centre + len * cosf(ang), ty = centre + len * sinf(ang);
    float x0 = fminf(centre, tx), y0 = fminf(centre, ty);

    u->needle_pts[0] = (lv_point_precise_t){centre - x0, centre - y0};
    u->needle_pts[1] = (lv_point_precise_t){tx - x0, ty - y0};
    lv_line_set_points_mutable(u->needle, u->needle_pts, 2);
    lv_obj_set_pos(u->needle, (int32_t)x0, (int32_t)y0);
}

static const float P10[] = {1, 10, 100, 1000};

static void update_widget(ui_widget_t *w, bool force)
{
    const roundGauge_widget_t *c = w->cfg;
    if (c->type == RG_WIDGET_TEXT || c->type == RG_WIDGET_IMAGE) {
        return; // статичные
    }
    float v = 0;
    bool ok = roundGauge_signal_get(w->sig.id, &v);
    bool first = force || !w->shown;
    w->shown = true;

    switch (c->type) {
    case RG_WIDGET_VALUE: {
        const float shown = v / c->div; // div = 1 по умолчанию; зоны ниже - по исходному значению
        int32_t key = ok ? (int32_t)lroundf(shown * P10[w->sig.decimals]) : INT32_MIN;
        if (first || key != w->key) {
            if (ok) {
                // snprintf из libc: встроенный sprintf LVGL не умеет %f.
                char txt[24];
                snprintf(txt, sizeof(txt), "%.*f", (int)w->sig.decimals, (double)shown);
                lv_label_set_text(w->obj, txt);
            } else {
                lv_label_set_text(w->obj, "--");
            }
            w->key = key;
        }
        if (c->zone_color) {
            int zone = ok ? zone_index(&w->sig, v) : -1;
            if (first || zone != w->zone) {
                w->zone = zone;
                lv_obj_set_style_text_color(w->obj, lv_color_hex(zone >= 0 ? w->sig.zones[zone].color : c->color), 0);
            }
        }
        break;
    }
    case RG_WIDGET_INDICATOR: {
        bool on = ok && (c->op == '<' ? v < c->threshold : v > c->threshold);
        if (on && c->blink) {
            on = ((esp_timer_get_time() / 1000 / BLINK_HALF_PERIOD_MS) & 1) == 0;
        }
        if (first || (int32_t)on != w->key) {
            lv_obj_set_hidden(w->obj, !on);
            w->key = on;
        }
        break;
    }
    case RG_WIDGET_BAR:
    case RG_WIDGET_ARC: {
        int32_t pos = ok ? to_res(w->sig.min, w->sig.max, v) : 0;
        if (first || pos != w->key) {
            if (c->type == RG_WIDGET_BAR) {
                lv_bar_set_value(w->obj, pos, LV_ANIM_OFF);
            } else {
                lv_arc_set_value(w->obj, pos);
            }
            w->key = pos;
        }
        if (c->zone_color) {
            int zone = ok ? zone_index(&w->sig, v) : -1;
            if (first || zone != w->zone) {
                w->zone = zone;
                lv_color_t col = lv_color_hex(zone >= 0 ? w->sig.zones[zone].color : c->color);
                if (c->type == RG_WIDGET_BAR) {
                    lv_obj_set_style_bg_color(w->obj, col, LV_PART_INDICATOR);
                } else {
                    lv_obj_set_style_arc_color(w->obj, col, LV_PART_INDICATOR);
                }
            }
        }
        break;
    }
    default:
        break;
    }
}

static void update_screen(ui_screen_t *u)
{
    if (u->cfg->type == RG_SCREEN_GMETER) {
        update_gmeter(u);
        bool first = !u->shown;
        u->shown = true;
        for (int i = 0; i < u->w_count; i++) {
            update_widget(&u->w[i], first);
        }
        return;
    }
    float v = 0;
    bool ok = roundGauge_signal_get(u->sig.id, &v);
    int32_t pos_key = ok ? to_res(u->sig.min, u->sig.max, v) : -1;

    if (!u->shown || pos_key != u->pos_key) {
        if (u->needle != NULL) {
            int32_t p = pos_key < 0 ? 0 : pos_key;
            if (u->needle_is_image) {
                lv_scale_set_image_needle_value(u->scale, u->needle, p);
            } else {
                needle_line_set(u, p);
            }
        }
        if (u->arc != NULL) {
            lv_arc_set_value(u->arc, pos_key < 0 ? 0 : pos_key);
            // Цвет заливки - цвет зоны, в которой сейчас значение.
            int zone = ok ? zone_index(&u->sig, v) : -1;
            if (zone != u->cur_zone || !u->shown) {
                u->cur_zone = zone;
                lv_obj_set_style_arc_color(u->arc, lv_color_hex(zone >= 0 ? u->sig.zones[zone].color : u->cfg->color),
                                           LV_PART_INDICATOR);
            }
        }
        u->pos_key = pos_key;
    }
    bool first = !u->shown;
    u->shown = true;
    for (int i = 0; i < u->w_count; i++) {
        update_widget(&u->w[i], first);
    }
}

void ui_screens_update(void)
{
    if (s_set != NULL && s_set->count > 0) {
        update_screen(&s_set->s[s_set->cur]);
    }
}
