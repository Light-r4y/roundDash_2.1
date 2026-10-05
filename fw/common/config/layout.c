#include "layout.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "layout_default.h"

static const char *TAG = "LAYOUT";

static roundGauge_layout_t s_layout;
static uint32_t s_generation;
static SemaphoreHandle_t s_mutex;

#define GREY 0x9E9E9E

// ------------------------------------------------------------------
// Разбор значений JSON. Отсутствующее или негодное поле оставляет значение
// по умолчанию.
// ------------------------------------------------------------------

static void get_float(const cJSON *obj, const char *key, float *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        *out = (float)v->valuedouble;
    }
}

static void get_int(const cJSON *obj, const char *key, int lo, int hi, int *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v)) {
        int x = (int)v->valuedouble;
        *out = x < lo ? lo : (x > hi ? hi : x);
    }
}

static void get_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v);
    }
}

static void get_str(const cJSON *obj, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(out, v->valuestring, size);
    }
}

// "#rrggbb" -> 0xRRGGBB; при ошибке значение не трогаем.
static void get_color(const cJSON *obj, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL || v->valuestring[0] != '#' || strlen(v->valuestring) != 7) {
        return;
    }
    char *end = NULL;
    unsigned long c = strtoul(v->valuestring + 1, &end, 16);
    if (end != NULL && *end == '\0') {
        *out = (uint32_t)c;
    }
}

// Имя файла (картинка, шрифт): только безопасные символы, без путей. Иначе - пусто.
static void get_file_name(const cJSON *obj, const char *key, char *out, size_t size)
{
    char tmp[32] = "";
    get_str(obj, key, tmp, sizeof(tmp));
    for (const char *p = tmp; *p; p++) {
        bool ok = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                  *p == '.' || *p == '_' || *p == '-';
        if (!ok || (p[0] == '.' && p[1] == '.')) {
            ESP_LOGW(TAG, "%s: bad file name \"%s\" ignored", key, tmp);
            return;
        }
    }
    if (tmp[0] != '\0') {
        strlcpy(out, tmp, size);
    }
}

static void parse_zones(const cJSON *arr, roundGauge_zone_t *zones, uint8_t *count, float lo, float hi, uint32_t color)
{
    const cJSON *it;
    *count = 0;
    cJSON_ArrayForEach(it, arr) {
        if (*count >= RG_LAYOUT_MAX_ZONES) break;
        roundGauge_zone_t *z = &zones[*count];
        z->from = lo;
        z->to = hi;
        z->color = color;
        get_float(it, "from", &z->from);
        get_float(it, "to", &z->to);
        get_color(it, "color", &z->color);
        if (z->to > z->from) {
            (*count)++;
        }
    }
}

// ------------------------------------------------------------------
// Сигнал
// ------------------------------------------------------------------

static bool parse_signal(const cJSON *j, roundGauge_signal_def_t *d)
{
    memset(d, 0, sizeof(*d));
    d->min = 0;
    d->max = 100;
    get_str(j, "name", d->name, sizeof(d->name));
    get_str(j, "title", d->title, sizeof(d->title));
    get_str(j, "unit", d->unit, sizeof(d->unit));
    get_float(j, "min", &d->min);
    get_float(j, "max", &d->max);
    if (d->name[0] == '\0' || !(d->max > d->min)) {
        ESP_LOGW(TAG, "signal needs \"name\" and min < max");
        return false;
    }
    int v = d->decimals;
    get_int(j, "decimals", 0, 3, &v);
    d->decimals = (uint8_t)v;
    parse_zones(cJSON_GetObjectItemCaseSensitive(j, "zones"), d->zones, &d->zone_count, d->min, d->max, 0xFFFFFF);
    return true;
}

const roundGauge_signal_def_t *roundGauge_layout_find_signal(const roundGauge_layout_t *l, const char *name)
{
    for (int i = 0; i < l->signal_count; i++) {
        if (strcmp(l->signals[i].name, name) == 0) {
            return &l->signals[i];
        }
    }
    return NULL;
}

// ------------------------------------------------------------------
// Дополнительные виджеты
// ------------------------------------------------------------------

static bool parse_widget(const cJSON *j, roundGauge_widget_t *w)
{
    memset(w, 0, sizeof(*w));
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        return false;
    }
    static const char *const names[] = {
        [RG_WIDGET_VALUE] = "value", [RG_WIDGET_TEXT] = "text", [RG_WIDGET_IMAGE] = "image",
        [RG_WIDGET_INDICATOR] = "indicator", [RG_WIDGET_BAR] = "bar", [RG_WIDGET_ARC] = "arc",
    };
    int t = -1;
    for (int i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++) {
        if (strcmp(type->valuestring, names[i]) == 0) {
            t = i;
        }
    }
    if (t < 0) {
        ESP_LOGW(TAG, "unknown widget type \"%s\"", type->valuestring);
        return false;
    }
    w->type = (roundGauge_widget_type_t)t;

    // Умолчания по типам.
    w->color = 0xFFFFFF;
    w->bg_color = 0x303030;
    w->decimals = -1;
    w->op = '>';
    w->angle = 270;
    w->rotation = 135;
    w->width = 12;
    switch (w->type) {
    case RG_WIDGET_VALUE:
        strlcpy(w->font, "48", sizeof(w->font));
        break;
    case RG_WIDGET_TEXT:
        strlcpy(w->font, "28", sizeof(w->font));
        w->color = GREY;
        break;
    case RG_WIDGET_INDICATOR:
        w->w = 40;
        w->color = 0xFF3030;
        break;
    case RG_WIDGET_BAR:
        w->w = 200;
        w->h = 16;
        w->color = 0x00C0FF;
        break;
    case RG_WIDGET_ARC:
        w->w = 120;
        w->color = 0x00C0FF;
        break;
    default:
        break;
    }

    int v;
    v = w->x; get_int(j, "x", -480, 480, &v); w->x = (int16_t)v;
    v = w->y; get_int(j, "y", -480, 480, &v); w->y = (int16_t)v;
    v = w->w; get_int(j, "w", 1, 480, &v);    w->w = (uint16_t)v;
    v = w->h; get_int(j, "h", 1, 480, &v);    w->h = (uint16_t)v;
    v = w->decimals; get_int(j, "decimals", -1, 3, &v); w->decimals = (int8_t)v;
    v = w->angle; get_int(j, "angle", 10, 360, &v);     w->angle = (uint16_t)v;
    v = w->rotation; get_int(j, "rotation", -360, 360, &v); w->rotation = (int16_t)v;
    v = w->width; get_int(j, "width", 2, 120, &v);      w->width = (uint8_t)v;

    get_str(j, "signal", w->signal, sizeof(w->signal));
    get_file_name(j, "font", w->font, sizeof(w->font));
    get_str(j, "text", w->text, sizeof(w->text));
    get_file_name(j, "image", w->image, sizeof(w->image));
    get_color(j, "color", &w->color);
    get_color(j, "bg_color", &w->bg_color);
    get_bool(j, "zone_color", &w->zone_color);
    get_bool(j, "blink", &w->blink);
    get_float(j, "threshold", &w->threshold);
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(j, "op");
    if (cJSON_IsString(op) && op->valuestring != NULL && (op->valuestring[0] == '>' || op->valuestring[0] == '<')) {
        w->op = op->valuestring[0];
    }
    return true;
}

static void add_widget(roundGauge_screen_t *s, const roundGauge_widget_t *w)
{
    if (s->widget_count < RG_LAYOUT_MAX_WIDGETS) {
        s->widgets[s->widget_count++] = *w;
    }
}

// ------------------------------------------------------------------
// Экран
// ------------------------------------------------------------------

static bool parse_screen(const cJSON *j, roundGauge_screen_t *s)
{
    memset(s, 0, sizeof(*s));

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(j, "type");
    if (!cJSON_IsString(type) || type->valuestring == NULL) {
        return false;
    }
    if (strcmp(type->valuestring, "dial") == 0) {
        s->type = RG_SCREEN_DIAL;
    } else if (strcmp(type->valuestring, "ring") == 0) {
        s->type = RG_SCREEN_RING;
    } else if (strcmp(type->valuestring, "number") == 0) {
        s->type = RG_SCREEN_NUMBER;
    } else {
        ESP_LOGW(TAG, "unknown screen type \"%s\"", type->valuestring);
        return false;
    }

    s->min = 0;
    s->max = 100;
    s->bg_color = 0x000000;
    s->color = 0xFFFFFF;
    s->text_color = 0xFFFFFF;
    s->angle = 270;
    s->rotation = 135;
    s->ticks = 41;
    s->major_every = 5;
    s->label_div = 1;
    s->needle_color = 0xFF8000;
    s->needle_width = 6;
    s->ring_width = 36;

    get_str(j, "signal", s->signal, sizeof(s->signal));
    if (s->signal[0] == '\0') {
        ESP_LOGW(TAG, "screen needs \"signal\"");
        return false;
    }

    // Свой диапазон экрана - необязателен; по умолчанию его даёт сигнал.
    const cJSON *mn = cJSON_GetObjectItemCaseSensitive(j, "min");
    const cJSON *mx = cJSON_GetObjectItemCaseSensitive(j, "max");
    if (cJSON_IsNumber(mn) && cJSON_IsNumber(mx) && mx->valuedouble > mn->valuedouble) {
        s->has_range = true;
        s->min = (float)mn->valuedouble;
        s->max = (float)mx->valuedouble;
    }

    get_float(j, "label_div", &s->label_div);
    if (!(s->label_div > 0)) {
        s->label_div = 1;
    }

    int v;
    v = s->angle;        get_int(j, "angle", 10, 360, &v);       s->angle = (uint16_t)v;
    v = s->rotation;     get_int(j, "rotation", -360, 360, &v);  s->rotation = (int16_t)v;
    v = s->ticks;        get_int(j, "ticks", 2, 101, &v);        s->ticks = (uint8_t)v;
    v = s->major_every;  get_int(j, "major_every", 1, 100, &v);  s->major_every = (uint8_t)v;
    v = s->needle_width; get_int(j, "needle_width", 1, 40, &v);  s->needle_width = (uint8_t)v;
    v = s->ring_width;   get_int(j, "ring_width", 4, 120, &v);   s->ring_width = (uint8_t)v;

    // Подписей на шкале не больше RG_LAYOUT_MAX_LABELS: растим шаг крупных делений.
    while ((s->ticks - 1) / s->major_every + 1 > RG_LAYOUT_MAX_LABELS && s->major_every < 100) {
        s->major_every++;
    }

    get_color(j, "bg_color", &s->bg_color);
    get_color(j, "color", &s->color);
    get_color(j, "text_color", &s->text_color);
    get_color(j, "needle_color", &s->needle_color);
    get_file_name(j, "bg_image", s->bg_image, sizeof(s->bg_image));
    get_file_name(j, "needle_image", s->needle_image, sizeof(s->needle_image));

    parse_zones(cJSON_GetObjectItemCaseSensitive(j, "zones"), s->zones, &s->zone_count, s->min, s->max, s->color);

    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "markers")) {
        if (s->marker_count >= RG_LAYOUT_MAX_MARKERS) break;
        roundGauge_marker_t *m = &s->markers[s->marker_count];
        m->value = s->min;
        m->color = s->color;
        get_float(it, "value", &m->value);
        get_color(it, "color", &m->color);
        s->marker_count++;
    }

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "widgets")) {
        roundGauge_widget_t w;
        if (parse_widget(it, &w)) {
            add_widget(s, &w);
        }
    }
    return true;
}

// ------------------------------------------------------------------
// Версия 1 -> 2: сигнал и текстовые виджеты берутся из полей экрана.
// ------------------------------------------------------------------

static void migrate_v1_screen(const cJSON *j, roundGauge_layout_t *l, roundGauge_screen_t *s)
{
    // Сигнал экрана - в таблицу, если такого там ещё нет.
    if (roundGauge_layout_find_signal(l, s->signal) == NULL && l->signal_count < RG_LAYOUT_MAX_SIGNALS) {
        roundGauge_signal_def_t *d = &l->signals[l->signal_count];
        memset(d, 0, sizeof(*d));
        strlcpy(d->name, s->signal, sizeof(d->name));
        get_str(j, "title", d->title, sizeof(d->title));
        get_str(j, "unit", d->unit, sizeof(d->unit));
        d->min = s->min;
        d->max = s->max;
        int dec = 0;
        get_int(j, "decimals", 0, 3, &dec);
        d->decimals = (uint8_t)dec;
        d->zone_count = s->zone_count;
        memcpy(d->zones, s->zones, sizeof(d->zones));
        l->signal_count++;
    }
    s->has_range = false;

    char title[24] = "", unit[12] = "";
    get_str(j, "title", title, sizeof(title));
    get_str(j, "unit", unit, sizeof(unit));
    bool show_value = true;
    get_bool(j, "show_value", &show_value);

    // Места те же, что рисовал прошивочный код версии 1.
    int y_title = s->type == RG_SCREEN_RING ? -70 : -90;
    int y_value = s->type == RG_SCREEN_DIAL ? 60 : 0;
    int y_unit = s->type == RG_SCREEN_DIAL ? 110 : 60;

    if (s->widget_count == 0) {
        roundGauge_widget_t w;
        const char *tpl[] = {"{\"type\":\"text\"}", "{\"type\":\"value\"}"};
        if (title[0]) {
            cJSON *t = cJSON_Parse(tpl[0]);
            parse_widget(t, &w);
            cJSON_Delete(t);
            w.y = (int16_t)y_title;
            strlcpy(w.text, title, sizeof(w.text));
            add_widget(s, &w);
        }
        if (show_value) {
            cJSON *t = cJSON_Parse(tpl[1]);
            parse_widget(t, &w);
            cJSON_Delete(t);
            w.y = (int16_t)y_value;
            w.color = s->text_color;
            add_widget(s, &w);
            if (unit[0]) {
                t = cJSON_Parse(tpl[0]);
                parse_widget(t, &w);
                cJSON_Delete(t);
                w.y = (int16_t)y_unit;
                strlcpy(w.text, unit, sizeof(w.text));
                add_widget(s, &w);
            }
        }
    }
}

static bool parse(const char *json, roundGauge_layout_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const cJSON *sigs = cJSON_GetObjectItemCaseSensitive(root, "signals");
    const bool legacy = !cJSON_IsArray(sigs);
    const cJSON *it;

    if (!legacy) {
        cJSON_ArrayForEach(it, sigs) {
            if (out->signal_count >= RG_LAYOUT_MAX_SIGNALS) {
                ESP_LOGW(TAG, "more than %d signals, extra ignored", RG_LAYOUT_MAX_SIGNALS);
                break;
            }
            if (parse_signal(it, &out->signals[out->signal_count])) {
                out->signal_count++;
            }
        }
    }

    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "screens")) {
        if (out->count >= RG_UI_MAX_SCREENS) {
            ESP_LOGW(TAG, "more than %d screens, extra ignored", RG_UI_MAX_SCREENS);
            break;
        }
        roundGauge_screen_t *s = &out->screens[out->count];
        if (!parse_screen(it, s)) {
            continue;
        }
        if (legacy) {
            // В версии 1 диапазон лежал на экране: parse_screen принял его как свой.
            migrate_v1_screen(it, out, s);
        }
        out->count++;
    }
    cJSON_Delete(root);
    return out->count > 0;
}

// ------------------------------------------------------------------
// NVS
// ------------------------------------------------------------------

static char *nvs_load_json(void)
{
    nvs_handle_t h;
    if (nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *buf = NULL;
    if (nvs_get_blob(h, RG_LAYOUT_NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len <= RG_LAYOUT_JSON_MAX) {
        buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, RG_LAYOUT_NVS_KEY, buf, &len) == ESP_OK) {
            buf[len] = '\0';
        } else {
            free(buf);
            buf = NULL;
        }
    }
    nvs_close(h);
    return buf;
}

static esp_err_t nvs_save_json(const char *json)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, RG_LAYOUT_NVS_KEY, json, strlen(json));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void set_current(const roundGauge_layout_t *l)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_layout = *l;
    s_generation++;
    xSemaphoreGive(s_mutex);
}

void roundGauge_layout_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);

    roundGauge_layout_t *tmp = malloc(sizeof(*tmp));
    configASSERT(tmp != NULL);
    char *json = nvs_load_json();
    if (json != NULL && parse(json, tmp)) {
        ESP_LOGI(TAG, "Layout from NVS: %u screens, %u signals", (unsigned)tmp->count, (unsigned)tmp->signal_count);
    } else {
        if (json != NULL) {
            ESP_LOGW(TAG, "Stored layout is broken, using built-in");
        }
        bool ok = parse(RG_LAYOUT_DEFAULT_JSON, tmp);
        configASSERT(ok);
        ESP_LOGI(TAG, "Built-in layout: %u screens", (unsigned)tmp->count);
    }
    free(json);
    set_current(tmp);
    free(tmp);
}

void roundGauge_layout_copy(roundGauge_layout_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_layout;
    xSemaphoreGive(s_mutex);
}

uint32_t roundGauge_layout_generation(void)
{
    return s_generation;
}

void roundGauge_layout_reload(void)
{
    s_generation++;
}

esp_err_t roundGauge_layout_apply_json(const char *json)
{
    size_t len = json ? strlen(json) : 0;
    if (len == 0 || len > RG_LAYOUT_JSON_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    roundGauge_layout_t *l = malloc(sizeof(*l));
    if (l == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (parse(json, l)) {
        err = nvs_save_json(json);
        if (err == ESP_OK) {
            set_current(l);
            ESP_LOGI(TAG, "Layout applied: %u screens, %u signals", (unsigned)l->count, (unsigned)l->signal_count);
        }
    }
    free(l);
    return err;
}

char *roundGauge_layout_json_dup(void)
{
    char *json = nvs_load_json();
    if (json == NULL) {
        json = strdup(RG_LAYOUT_DEFAULT_JSON);
    }
    return json;
}

esp_err_t roundGauge_layout_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, RG_LAYOUT_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err == ESP_OK) {
        roundGauge_layout_t *tmp = malloc(sizeof(*tmp));
        if (tmp == NULL) {
            return ESP_ERR_NO_MEM;
        }
        bool ok = parse(RG_LAYOUT_DEFAULT_JSON, tmp);
        configASSERT(ok);
        set_current(tmp);
        free(tmp);
    }
    return err;
}
