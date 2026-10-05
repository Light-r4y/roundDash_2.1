#include "ui_task.h"
#include "conf.h"
#include "board.h"
#include "esp_lvgl_port.h"
#include <stdatomic.h>
#include <stdio.h>
#include "layout.h"
#include "settings.h"
#include "signals.h"
#include "ui_screens.h"
#include "webcfg_task.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "UI";

static lv_display_t *s_disp;

static void lvgl_start(void)
{
    const lvgl_port_cfg_t port_cfg = {
        .task_priority = RG_LVGL_TASK_PRIORITY,
        .task_stack = RG_LVGL_TASK_STACK,
        .task_affinity = RG_LVGL_TASK_CORE,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    /*
     * Без разрывов: LVGL рисует прямо в кадровые буферы панели (два, в PSRAM)
     * в direct mode, esp_lvgl_port меняет их по окончании кадра. Буферы
     * выделяет драйвер панели (board.c), поэтому buffer_size — целый экран.
     */
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = roundGauge_board_lcd_panel(),
        .buffer_size = RG_LCD_H_RES * RG_LCD_V_RES,
        .double_buffer = true,
        .hres = RG_LCD_H_RES,
        .vres = RG_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .direct_mode = true,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = true,
        },
    };
    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    configASSERT(s_disp != NULL);

    // Тач в LVGL не регистрируем: свайпы распознаёт отдельная задача (tasks/touch), а
    // индев LVGL читает тач только между кадрами и судит о жесте по скорости между
    // соседними чтениями - при медленной отрисовке свайпы терялись.
}

static atomic_int s_switch_req; // накопленные запросы переключения от кнопок

void roundGauge_ui_switch_screen(int dir)
{
    atomic_fetch_add(&s_switch_req, dir);
}

static void media_mount(void)
{
    esp_vfs_spiffs_conf_t fs = {
        .base_path = RG_MEDIA_BASE_PATH,
        .partition_label = RG_MEDIA_PARTITION,
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&fs);
    if (err != ESP_OK) {
        // Не страшно: без картинок экраны рисуются примитивами.
        ESP_LOGW(TAG, "Mount %s failed: %s", RG_MEDIA_PARTITION, esp_err_to_name(err));
    }
}

#if RG_UI_SHOW_FPS
// Метрики по событиям дисплея. Если картинка не меняется, LVGL ничего не рисует -
// тогда fps честно 0.
//   r  - рисование: от начала кадра до flush последней области;
//   w  - ожидание в flush последней области: esp_lvgl_port ждёт vsync панели
//        (RENDER_READY приходит уже после него), т.е. это не работа процессора;
//   Kpx - сколько пикселей перерисовано за кадр (сумма областей flush).
static int64_t s_render_t0, s_flush_t;
static uint32_t s_frames;
static int64_t s_draw_us, s_wait_us;
static uint64_t s_px;
static lv_obj_t *s_fps_label;

static void render_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_RENDER_START:
        s_render_t0 = now;
        s_flush_t = now;
        break;
    case LV_EVENT_FLUSH_START:
        s_flush_t = now;
        s_px += (uint64_t)lv_area_get_size((const lv_area_t *)lv_event_get_param(e));
        break;
    default: // LV_EVENT_RENDER_READY
        s_draw_us += s_flush_t - s_render_t0;
        s_wait_us += now - s_flush_t;
        s_frames++;
        break;
    }
}

static void fps_init(void)
{
    lv_display_add_event_cb(s_disp, render_cb, LV_EVENT_RENDER_START, NULL);
    lv_display_add_event_cb(s_disp, render_cb, LV_EVENT_FLUSH_START, NULL);
    lv_display_add_event_cb(s_disp, render_cb, LV_EVENT_RENDER_READY, NULL);
    // На верхнем слое: остаётся при смене экранов. Сверху круга, где ещё есть место.
    s_fps_label = lv_label_create(lv_layer_top());
    lv_obj_set_style_text_color(s_fps_label, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_text_align(s_fps_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_fps_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_fps_label, LV_OPA_70, 0);
    lv_obj_align(s_fps_label, LV_ALIGN_TOP_MID, 0, 14);
    lv_label_set_text(s_fps_label, "");
}

static void fps_update(void)
{
    static int64_t last_us;
    int64_t now = esp_timer_get_time();
    if (now - last_us < 1000000) {
        return;
    }
    float secs = (float)(now - last_us) / 1e6f;
    uint32_t frames = s_frames;
    float n = frames ? (float)frames : 1.0f;
    // snprintf из libc: встроенный sprintf LVGL не умеет %f.
    char txt[64];
    snprintf(txt, sizeof(txt), "%d/%d  %u fps\nr %.0f  w %.0f ms  %.0f Kpx", ui_screens_current() + 1,
             ui_screens_count(), (unsigned)((float)frames / secs + 0.5f), (double)((float)s_draw_us / 1000.0f / n),
             (double)((float)s_wait_us / 1000.0f / n), (double)((float)s_px / 1000.0f / n));
    lv_label_set_text(s_fps_label, txt);
    s_frames = 0;
    s_draw_us = s_wait_us = 0;
    s_px = 0;
    last_us = now;
}
#endif

// ---------------------------------------------------------------------------
// Точка доступа на экране: карточка с данными для подключения на RG_AP_CARD_MS и
// постоянный значок Wi-Fi с числом подключённых. Верхний слой - виден на любом экране.
// ---------------------------------------------------------------------------
static lv_obj_t *s_ap_card, *s_ap_title, *s_ap_l1, *s_ap_l2, *s_ap_l3, *s_ap_badge;

static lv_obj_t *ap_label(lv_obj_t *parent, int y, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_width(l, 320);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    lv_label_set_text(l, "");
    return l;
}

// Короткое сообщение (например, "Password reset"): ставят из любой задачи, показывает ui_task.
static char s_toast_text[48];
static volatile bool s_toast_new;
static portMUX_TYPE s_toast_lock = portMUX_INITIALIZER_UNLOCKED;
static lv_obj_t *s_toast;

void roundGauge_ui_toast(const char *text)
{
    portENTER_CRITICAL(&s_toast_lock);
    strlcpy(s_toast_text, text, sizeof(s_toast_text));
    s_toast_new = true;
    portEXIT_CRITICAL(&s_toast_lock);
}

static void ap_overlay_init(void)
{
    lv_obj_t *top = lv_layer_top();
    s_toast = lv_label_create(top);
    lv_obj_set_style_text_font(s_toast, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_toast, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x0B1015), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x00C0FF), 0);
    lv_obj_set_style_border_width(s_toast, 2, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_pad_all(s_toast, 10, 0);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 150);
    lv_obj_set_hidden(s_toast, true);

    s_ap_card = lv_obj_create(top);
    lv_obj_set_size(s_ap_card, 350, 214);
    lv_obj_center(s_ap_card);
    lv_obj_set_clickable(s_ap_card, false);
    lv_obj_set_scrollable(s_ap_card, false);
    lv_obj_set_style_bg_color(s_ap_card, lv_color_hex(0x0B1015), 0);
    lv_obj_set_style_bg_opa(s_ap_card, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_ap_card, lv_color_hex(0x00C0FF), 0);
    lv_obj_set_style_border_width(s_ap_card, 2, 0);
    lv_obj_set_style_radius(s_ap_card, 18, 0);
    lv_obj_set_style_pad_all(s_ap_card, 8, 0);
    s_ap_title = ap_label(s_ap_card, 6, lv_color_hex(0x00C0FF));
    s_ap_l1 = ap_label(s_ap_card, 52, lv_color_white());
    s_ap_l2 = ap_label(s_ap_card, 94, lv_color_white());
    s_ap_l3 = ap_label(s_ap_card, 136, lv_color_hex(0x9E9E9E));
    lv_obj_set_hidden(s_ap_card, true);

    s_ap_badge = lv_label_create(top);
    lv_obj_set_style_text_font(s_ap_badge, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_ap_badge, lv_color_hex(0x00C0FF), 0);
    lv_obj_align(s_ap_badge, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_hidden(s_ap_badge, true);
}

static void ap_overlay_update(void)
{
    static roundGauge_ap_state_t last_state = RG_AP_OFF;
    static int last_clients = -1;
    static int64_t up_ms;
    static bool card_shown;

    roundGauge_ap_info_t info;
    roundGauge_webcfg_get_ap_info(&info);
    int64_t now = esp_timer_get_time() / 1000;

    static int64_t toast_until;
    if (s_toast_new) {
        char txt[sizeof(s_toast_text)];
        portENTER_CRITICAL(&s_toast_lock);
        strlcpy(txt, s_toast_text, sizeof(txt));
        s_toast_new = false;
        portEXIT_CRITICAL(&s_toast_lock);
        lv_label_set_text(s_toast, txt);
        lv_obj_set_hidden(s_toast, false);
        toast_until = now + RG_TOAST_MS;
    } else if (toast_until != 0 && now >= toast_until) {
        lv_obj_set_hidden(s_toast, true);
        toast_until = 0;
    }

    if (info.state != last_state) {
        last_state = info.state;
        if (info.state == RG_AP_STARTING) {
            lv_label_set_text(s_ap_title, LV_SYMBOL_WIFI "  Wi-Fi");
            lv_label_set_text(s_ap_l1, "Starting...");
            lv_label_set_text(s_ap_l2, "");
            lv_label_set_text(s_ap_l3, "");
        } else if (info.state == RG_AP_UP) {
            up_ms = now;
            char pass[80];
            snprintf(pass, sizeof(pass), info.password[0] ? "Pass: %s" : "Open network", info.password);
            lv_label_set_text(s_ap_title, LV_SYMBOL_WIFI "  Wi-Fi");
            lv_label_set_text(s_ap_l1, info.ssid);
            lv_label_set_text(s_ap_l2, pass);
            lv_label_set_text(s_ap_l3, "http://192.168.4.1");
        }
    }

    bool card = info.state == RG_AP_STARTING || (info.state == RG_AP_UP && now - up_ms < RG_AP_CARD_MS);
    if (card != card_shown) {
        card_shown = card;
        lv_obj_set_hidden(s_ap_card, !card);
    }

    if (info.state == RG_AP_UP) {
        if (info.clients != last_clients) {
            last_clients = info.clients;
            char txt[24];
            snprintf(txt, sizeof(txt), LV_SYMBOL_WIFI " %u", (unsigned)info.clients);
            lv_label_set_text(s_ap_badge, txt);
        }
        lv_obj_set_hidden(s_ap_badge, false);
    }
}

static void layout_rebuild(void)
{
    // ~2.5 КБ - в куче, не на стеке задачи.
    roundGauge_layout_t *l = malloc(sizeof(*l));
    if (l == NULL) {
        ESP_LOGE(TAG, "no memory for layout copy");
        return;
    }
    roundGauge_layout_copy(l);
    lvgl_port_lock(0);
    ui_screens_rebuild(l);
    lvgl_port_unlock();
    free(l);
}

static void ui_task(void *arg)
{
    lvgl_start();
    media_mount();

    uint32_t layout_gen = roundGauge_layout_generation();
    layout_rebuild();
    lvgl_port_lock(0);
    ap_overlay_init();
#if RG_UI_SHOW_FPS
    fps_init();
#endif
    lvgl_port_unlock();

    // Подсветка после первого кадра — без мелькания мусора из PSRAM.
    vTaskDelay(pdMS_TO_TICKS(100));
    roundGauge_display_settings_t ds;
    roundGauge_settings_get_display(&ds);
    ESP_ERROR_CHECK(roundGauge_board_backlight_set(ds.brightness));
    ESP_LOGI(TAG, "UI task started");

    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RG_UI_TASK_PERIOD_MS));

        // Layout поменяли (через веб) - пересобрать экраны.
        uint32_t gen = roundGauge_layout_generation();
        if (gen != layout_gen) {
            layout_gen = gen;
            layout_rebuild();
        }

        roundGauge_signal_sim_step((uint32_t)(esp_timer_get_time() / 1000)); // только при включённом "демо"

        int sw = atomic_exchange(&s_switch_req, 0);

        lvgl_port_lock(0);
        for (; sw > 0; sw--) {
            ui_screens_switch(+1);
        }
        for (; sw < 0; sw++) {
            ui_screens_switch(-1);
        }
        ui_screens_update();
        {
            static int64_t last_ap_ms;
            int64_t now_ms = esp_timer_get_time() / 1000;
            if (now_ms - last_ap_ms >= 250) {
                last_ap_ms = now_ms;
                ap_overlay_update();
            }
        }
#if RG_UI_SHOW_FPS
        fps_update();
#endif
        lvgl_port_unlock();
    }
}

void roundGauge_ui_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(ui_task, "ui_task", RG_UI_TASK_STACK, NULL,
                                            RG_UI_TASK_PRIORITY, NULL, RG_UI_TASK_CORE);
    configASSERT(ok == pdPASS);
}
