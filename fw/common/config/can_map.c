#include "can_map.h"
#include "can_map_default.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "CAN_MAP";

static roundGauge_can_map_t s_map;
static uint32_t s_generation;
static SemaphoreHandle_t s_mutex;

// ------------------------------------------------------------------
// Извлечение битов (семантика DBC)
// ------------------------------------------------------------------

static inline int frame_bit(const uint8_t data[8], int n)
{
    return (data[n >> 3] >> (n & 7)) & 1;
}

bool roundGauge_can_extract(const uint8_t data[8], uint8_t start, uint8_t len, roundGauge_can_order_t order,
                            bool is_signed, int64_t *raw)
{
    if (len < 1 || len > 32 || start > 63) {
        return false;
    }
    uint64_t v = 0;
    if (order == RG_CAN_ORDER_INTEL) {
        // Младший бит - start, дальше вверх по нумерации кадра.
        if ((int)start + len > 64) {
            return false;
        }
        for (int i = 0; i < len; i++) {
            v |= (uint64_t)frame_bit(data, start + i) << i;
        }
    } else {
        // Старший бит - start; идём вниз внутри байта, после бита 0 - на бит 7 следующего байта.
        int p = start;
        for (int i = 0; i < len; i++) {
            if (p < 0 || p > 63) {
                return false;
            }
            v = (v << 1) | (uint64_t)frame_bit(data, p);
            p = (p & 7) == 0 ? p + 15 : p - 1;
        }
    }
    if (is_signed && len < 64 && (v & (1ULL << (len - 1)))) {
        v |= ~0ULL << len; // знаковое расширение
    }
    *raw = (int64_t)v;
    return true;
}

// ------------------------------------------------------------------
// JSON -> таблица
// ------------------------------------------------------------------

static bool parse_id(const cJSON *v, uint32_t *out)
{
    if (cJSON_IsNumber(v) && v->valuedouble >= 0 && v->valuedouble <= 536870911.0) {
        *out = (uint32_t)v->valuedouble;
        return true;
    }
    if (cJSON_IsString(v) && v->valuestring != NULL && v->valuestring[0] != '\0') {
        char *end = NULL;
        unsigned long id = strtoul(v->valuestring, &end, 0); // "0x201" или "513"
        if (end != NULL && *end == '\0' && id <= 0x1FFFFFFFUL) {
            *out = (uint32_t)id;
            return true;
        }
    }
    return false;
}

static bool parse_entry(const cJSON *j, roundGauge_can_entry_t *e)
{
    memset(e, 0, sizeof(*e));
    e->factor = 1;
    e->timeout_ms = RG_CAN_TIMEOUT_DEFAULT_MS;

    const cJSON *sig = cJSON_GetObjectItemCaseSensitive(j, "signal");
    if (!cJSON_IsString(sig) || sig->valuestring == NULL || sig->valuestring[0] == '\0') {
        return false;
    }
    strlcpy(e->signal, sig->valuestring, sizeof(e->signal));

    if (!parse_id(cJSON_GetObjectItemCaseSensitive(j, "id"), &e->id)) {
        ESP_LOGW(TAG, "%s: bad id", e->signal);
        return false;
    }
    const cJSON *ext = cJSON_GetObjectItemCaseSensitive(j, "ext");
    e->ext = cJSON_IsTrue(ext);
    if (!e->ext && e->id > 0x7FF) {
        ESP_LOGW(TAG, "%s: id 0x%lx needs ext=true", e->signal, (unsigned long)e->id);
        return false;
    }

    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "start");
    if (!cJSON_IsNumber(v) || v->valuedouble < 0 || v->valuedouble > 63) {
        return false;
    }
    e->start = (uint8_t)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "len");
    if (!cJSON_IsNumber(v) || v->valuedouble < 1 || v->valuedouble > 32) {
        return false;
    }
    e->len = (uint8_t)v->valuedouble;

    const cJSON *order = cJSON_GetObjectItemCaseSensitive(j, "order");
    e->order = (cJSON_IsString(order) && order->valuestring != NULL && strcmp(order->valuestring, "motorola") == 0)
                   ? RG_CAN_ORDER_MOTOROLA : RG_CAN_ORDER_INTEL;
    e->is_signed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "signed"));

    // Поле должно целиком помещаться в 8 байт - иначе привязка никогда не сработает.
    uint8_t probe[8] = {0};
    int64_t tmp;
    if (!roundGauge_can_extract(probe, e->start, e->len, e->order, e->is_signed, &tmp)) {
        ESP_LOGW(TAG, "%s: field start=%u len=%u does not fit into 8 bytes", e->signal, e->start, e->len);
        return false;
    }

    v = cJSON_GetObjectItemCaseSensitive(j, "factor");
    if (cJSON_IsNumber(v)) e->factor = (float)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "offset");
    if (cJSON_IsNumber(v)) e->offset = (float)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "timeout");
    if (cJSON_IsNumber(v)) {
        double t = v->valuedouble;
        e->timeout_ms = (uint16_t)(t < 100 ? 100 : (t > 60000 ? 60000 : t));
    }
    return true;
}

// false - JSON не разобрался; иначе true, при непустом map без годных записей - ok=false.
static bool parse(const char *json, roundGauge_can_map_t *out, bool *ok)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "map");
    *ok = cJSON_IsArray(arr);
    int total = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        total++;
        if (out->count >= RG_CAN_MAP_MAX) {
            ESP_LOGW(TAG, "more than %d entries, extra ignored", RG_CAN_MAP_MAX);
            break;
        }
        roundGauge_can_entry_t *e = &out->e[out->count];
        if (!parse_entry(it, e)) {
            continue;
        }
        // Одна запись на сигнал: повтор имени игнорируем.
        bool dup = false;
        for (int i = 0; i < out->count; i++) {
            if (strcmp(out->e[i].signal, e->signal) == 0) {
                dup = true;
            }
        }
        if (!dup) {
            out->count++;
        }
    }
    if (total > 0 && out->count == 0) {
        *ok = false;
    }
    cJSON_Delete(root);
    return true;
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
    if (nvs_get_blob(h, RG_CAN_MAP_NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len <= RG_CAN_MAP_JSON_MAX) {
        buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, RG_CAN_MAP_NVS_KEY, buf, &len) == ESP_OK) {
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
    err = nvs_set_blob(h, RG_CAN_MAP_NVS_KEY, json, strlen(json));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void set_current(const roundGauge_can_map_t *m)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_map = *m;
    s_generation++;
    xSemaphoreGive(s_mutex);
}

void roundGauge_can_map_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);

    roundGauge_can_map_t *tmp = calloc(1, sizeof(*tmp));
    configASSERT(tmp != NULL);
    char *json = nvs_load_json();
    bool ok = false;
    if (json != NULL && parse(json, tmp, &ok) && ok) {
        ESP_LOGI(TAG, "CAN map from NVS: %u entries", (unsigned)tmp->count);
    } else {
        if (json != NULL) {
            ESP_LOGW(TAG, "Stored CAN map is broken, using the rusEFI preset");
        }
        memset(tmp, 0, sizeof(*tmp));
        ok = false;
        // Нет сохранённой таблицы (или она битая): пресет rusEFI. Явно сохранённая пустая таблица
        // разбирается выше и остаётся пустой.
        if (!parse(RG_CAN_MAP_DEFAULT_JSON, tmp, &ok) || !ok) {
            memset(tmp, 0, sizeof(*tmp));
        } else {
            ESP_LOGI(TAG, "CAN map: built-in rusEFI preset, %u entries", (unsigned)tmp->count);
        }
    }
    free(json);
    set_current(tmp);
    free(tmp);
}

void roundGauge_can_map_copy(roundGauge_can_map_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_map;
    xSemaphoreGive(s_mutex);
}

uint32_t roundGauge_can_map_generation(void)
{
    return s_generation;
}

esp_err_t roundGauge_can_map_apply_json(const char *json)
{
    size_t len = json ? strlen(json) : 0;
    if (len == 0 || len > RG_CAN_MAP_JSON_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    roundGauge_can_map_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = false;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (parse(json, m, &ok) && ok) {
        err = nvs_save_json(json);
        if (err == ESP_OK) {
            set_current(m);
            ESP_LOGI(TAG, "CAN map applied: %u entries", (unsigned)m->count);
        }
    }
    free(m);
    return err;
}

esp_err_t roundGauge_can_map_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, RG_CAN_MAP_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }
    roundGauge_can_map_t *m = calloc(1, sizeof(*m));
    if (m == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = false;
    if (parse(RG_CAN_MAP_DEFAULT_JSON, m, &ok) && ok) {
        set_current(m);
        ESP_LOGI(TAG, "CAN map reset to the rusEFI preset: %u entries", (unsigned)m->count);
    } else {
        err = ESP_FAIL;
    }
    free(m);
    return err;
}

char *roundGauge_can_map_json_dup(void)
{
    char *json = nvs_load_json();
    if (json == NULL) {
        json = strdup(RG_CAN_MAP_DEFAULT_JSON); // сохранённой нет - действует пресет rusEFI
    }
    return json;
}
