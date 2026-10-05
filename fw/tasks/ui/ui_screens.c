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
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "UI";

// Внутреннее разрешение шкалы, дуги и полосы: значения сигнала приводятся к 0..RES,
// чтобы стрелка двигалась плавно при любых min/max (виджеты LVGL целочисленные).
#define RES 1000
#define NEEDLE_MARGIN 55
#define BLINK_HALF_PERIOD_MS 250 // мигание 2 Гц
#define MAX_FONTS (RG_UI_MAX_SCREENS * RG_LAYOUT_MAX_WIDGETS)

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

static void build_dial(ui_screen_t *u)
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
    lv_obj_set_style_text_font(scale, &lv_font_montserrat_28, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, lv_color_hex(c->text_color), LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 22, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 10, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 2, LV_PART_ITEMS);
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

    // Стрелка: картинка из media (смотрит вправо, ось вращения слева по центру)
    // либо, если картинки нет, линия.
    if (c->needle_image[0] && media_file_exists(c->needle_image)) {
        char src[48];
        media_src(c->needle_image, src, sizeof(src));
        lv_obj_t *img = lv_image_create(scale);
        lv_image_set_src(img, src);
        lv_obj_update_layout(img);
        int h = lv_obj_get_height(img);
        lv_image_set_pivot(img, 0, h / 2);
        lv_obj_set_pos(img, size / 2, size / 2 - h / 2);
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
        build_dial(u);
        break;
    case RG_SCREEN_RING:
        build_ring(u);
        break;
    case RG_SCREEN_NUMBER:
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
        int32_t key = ok ? (int32_t)lroundf(v * P10[w->sig.decimals]) : INT32_MIN;
        if (first || key != w->key) {
            if (ok) {
                // snprintf из libc: встроенный sprintf LVGL не умеет %f.
                char txt[24];
                snprintf(txt, sizeof(txt), "%.*f", (int)w->sig.decimals, (double)v);
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
