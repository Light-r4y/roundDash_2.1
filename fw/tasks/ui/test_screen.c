#include "test_screen.h"
#include <stdio.h>
#include "esp_app_desc.h"
#include "lvgl.h"

#define SCALE_SIZE 440
#define SCALE_MIN 0
#define SCALE_MAX 100
#define NEEDLE_LEN 175
#define SWEEP_MS 2000
#define TOUCH_DOT_SIZE 36

static lv_obj_t *s_scale;
static lv_obj_t *s_needle;
static lv_obj_t *s_value_label;
static lv_obj_t *s_touch_dot;

static void needle_anim_cb(void *var, int32_t value)
{
    lv_scale_set_line_needle_value(s_scale, s_needle, NEEDLE_LEN, value);
    lv_label_set_text_fmt(s_value_label, "%d", (int)value);
}

static void touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_obj_set_pos(s_touch_dot, p.x - TOUCH_DOT_SIZE / 2, p.y - TOUCH_DOT_SIZE / 2);
        lv_obj_set_hidden(s_touch_dot, false);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_hidden(s_touch_dot, true);
    }
}

void test_screen_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scr, lv_color_white(), 0);
    lv_obj_add_event_cb(scr, touch_event_cb, LV_EVENT_ALL, NULL);

    // Шкала на весь круглый экран: 270°, разрыв снизу.
    s_scale = lv_scale_create(scr);
    lv_obj_set_size(s_scale, SCALE_SIZE, SCALE_SIZE);
    lv_obj_center(s_scale);
    lv_obj_set_clickable(s_scale, false);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(s_scale, SCALE_MIN, SCALE_MAX);
    lv_scale_set_total_tick_count(s_scale, 51);
    lv_scale_set_major_tick_every(s_scale, 5);
    lv_scale_set_label_show(s_scale, true);
    lv_scale_set_angle_range(s_scale, 270);
    lv_scale_set_rotation(s_scale, 135);
    lv_obj_set_style_bg_opa(s_scale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_font(s_scale, &lv_font_montserrat_28, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 22, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 10, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_scale, 4, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_scale, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_scale, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_scale, lv_palette_main(LV_PALETTE_GREY), LV_PART_ITEMS);
    lv_obj_set_style_arc_width(s_scale, 0, LV_PART_MAIN);

    s_needle = lv_line_create(s_scale);
    lv_obj_set_style_line_width(s_needle, 6, 0);
    lv_obj_set_style_line_rounded(s_needle, true, 0);
    lv_obj_set_style_line_color(s_needle, lv_palette_main(LV_PALETTE_ORANGE), 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "roundGauge");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -70);

    s_value_label = lv_label_create(scr);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, 0);
    lv_obj_align(s_value_label, LV_ALIGN_CENTER, 0, 70);

    lv_obj_t *version = lv_label_create(scr);
    lv_label_set_text_fmt(version, "v%s", esp_app_get_description()->version);
    lv_obj_set_style_text_color(version, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_align(version, LV_ALIGN_CENTER, 0, 130);

    s_touch_dot = lv_obj_create(scr);
    lv_obj_set_size(s_touch_dot, TOUCH_DOT_SIZE, TOUCH_DOT_SIZE);
    lv_obj_set_style_radius(s_touch_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_touch_dot, lv_palette_main(LV_PALETTE_CYAN), 0);
    lv_obj_set_style_border_width(s_touch_dot, 0, 0);
    lv_obj_set_clickable(s_touch_dot, false);
    lv_obj_set_hidden(s_touch_dot, true);

    // Стрелка ходит туда-обратно по всей шкале.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_needle);
    lv_anim_set_exec_cb(&a, needle_anim_cb);
    lv_anim_set_values(&a, SCALE_MIN, SCALE_MAX);
    lv_anim_set_duration(&a, SWEEP_MS);
    lv_anim_set_reverse_duration(&a, SWEEP_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}
