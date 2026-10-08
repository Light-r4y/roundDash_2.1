#include "alerts.h"
#include "alerts_default.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ALERTS";

static roundGauge_alerts_t s_alerts;
static uint32_t s_generation;
static SemaphoreHandle_t s_mutex;

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool parse_rule(const cJSON *j, roundGauge_alert_rule_t *r)
{
    memset(r, 0, sizeof(*r));
    r->op = '>';
    r->count = 1;
    r->on_ms = 150;
    r->off_ms = 150;
    r->enabled = true;

    const cJSON *sig = cJSON_GetObjectItemCaseSensitive(j, "signal");
    if (!cJSON_IsString(sig) || sig->valuestring == NULL || sig->valuestring[0] == '\0') {
        return false;
    }
    strlcpy(r->signal, sig->valuestring, sizeof(r->signal));

    const cJSON *op = cJSON_GetObjectItemCaseSensitive(j, "op");
    if (cJSON_IsString(op) && op->valuestring != NULL) {
        if (strcmp(op->valuestring, "<") == 0) {
            r->op = '<';
        } else if (strcmp(op->valuestring, ">") != 0) {
            ESP_LOGW(TAG, "%s: bad op \"%s\"", r->signal, op->valuestring);
            return false;
        }
    }
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "value");
    if (!cJSON_IsNumber(v)) {
        return false;
    }
    r->value = (float)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(j, "hyst");
    if (cJSON_IsNumber(v) && v->valuedouble > 0) {
        r->hyst = (float)v->valuedouble;
    }
    const cJSON *pat = cJSON_GetObjectItemCaseSensitive(j, "pattern");
    if (cJSON_IsString(pat) && pat->valuestring != NULL && strcmp(pat->valuestring, "continuous") == 0) {
        r->pattern = RG_ALERT_CONTINUOUS;
    }
    v = cJSON_GetObjectItemCaseSensitive(j, "count");
    if (cJSON_IsNumber(v)) r->count = (uint8_t)clampf((float)v->valuedouble, 1, 9);
    v = cJSON_GetObjectItemCaseSensitive(j, "on");
    if (cJSON_IsNumber(v)) r->on_ms = (uint16_t)clampf((float)v->valuedouble, 20, 2000);
    v = cJSON_GetObjectItemCaseSensitive(j, "off");
    if (cJSON_IsNumber(v)) r->off_ms = (uint16_t)clampf((float)v->valuedouble, 20, 2000);
    v = cJSON_GetObjectItemCaseSensitive(j, "repeat");
    if (cJSON_IsNumber(v)) r->repeat_s = (uint16_t)clampf((float)v->valuedouble, 0, 3600);
    const cJSON *en = cJSON_GetObjectItemCaseSensitive(j, "enabled");
    if (cJSON_IsBool(en)) r->enabled = cJSON_IsTrue(en);
    return true;
}

// false - JSON не разобрался; иначе true, при непустом "rules" без годных правил - ok=false.
static bool parse(const char *json, roundGauge_alerts_t *out, bool *ok)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "rules");
    *ok = cJSON_IsArray(arr);
    int total = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        total++;
        if (out->count >= RG_ALERTS_MAX) {
            ESP_LOGW(TAG, "more than %d rules, extra ignored", RG_ALERTS_MAX);
            break;
        }
        if (parse_rule(it, &out->r[out->count])) {
            out->count++;
        }
    }
    if (total > 0 && out->count == 0) {
        *ok = false;
    }
    cJSON_Delete(root);
    return true;
}

static char *nvs_load_json(void)
{
    nvs_handle_t h;
    if (nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return NULL;
    }
    size_t len = 0;
    char *buf = NULL;
    if (nvs_get_blob(h, RG_ALERTS_NVS_KEY, NULL, &len) == ESP_OK && len > 0 && len <= RG_ALERTS_JSON_MAX) {
        buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, RG_ALERTS_NVS_KEY, buf, &len) == ESP_OK) {
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
    err = nvs_set_blob(h, RG_ALERTS_NVS_KEY, json, strlen(json));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

static void set_current(const roundGauge_alerts_t *a)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_alerts = *a;
    s_generation++;
    xSemaphoreGive(s_mutex);
}

void roundGauge_alerts_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);

    roundGauge_alerts_t *tmp = calloc(1, sizeof(*tmp));
    configASSERT(tmp != NULL);
    char *json = nvs_load_json();
    bool ok = false;
    if (json != NULL && parse(json, tmp, &ok) && ok) {
        ESP_LOGI(TAG, "Alert rules from NVS: %u", (unsigned)tmp->count);
    } else {
        if (json != NULL) {
            ESP_LOGW(TAG, "Stored alert rules are broken, using the built-in ones");
        }
        memset(tmp, 0, sizeof(*tmp));
        ok = false;
        if (!parse(RG_ALERTS_DEFAULT_JSON, tmp, &ok) || !ok) {
            memset(tmp, 0, sizeof(*tmp));
        } else {
            ESP_LOGI(TAG, "Built-in alert rules: %u", (unsigned)tmp->count);
        }
    }
    free(json);
    set_current(tmp);
    free(tmp);
}

void roundGauge_alerts_copy(roundGauge_alerts_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_alerts;
    xSemaphoreGive(s_mutex);
}

uint32_t roundGauge_alerts_generation(void)
{
    return s_generation;
}

esp_err_t roundGauge_alerts_apply_json(const char *json)
{
    size_t len = json ? strlen(json) : 0;
    if (len == 0 || len > RG_ALERTS_JSON_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    roundGauge_alerts_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = false;
    esp_err_t err = ESP_ERR_INVALID_ARG;
    if (parse(json, a, &ok) && ok) {
        err = nvs_save_json(json);
        if (err == ESP_OK) {
            set_current(a);
            ESP_LOGI(TAG, "Alert rules applied: %u", (unsigned)a->count);
        }
    }
    free(a);
    return err;
}

char *roundGauge_alerts_json_dup(void)
{
    char *json = nvs_load_json();
    if (json == NULL) {
        json = strdup(RG_ALERTS_DEFAULT_JSON);
    }
    return json;
}

esp_err_t roundGauge_alerts_reset(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, RG_ALERTS_NVS_KEY);
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
    roundGauge_alerts_t *a = calloc(1, sizeof(*a));
    if (a == NULL) {
        return ESP_ERR_NO_MEM;
    }
    bool ok = false;
    if (parse(RG_ALERTS_DEFAULT_JSON, a, &ok) && ok) {
        set_current(a);
    } else {
        err = ESP_FAIL;
    }
    free(a);
    return err;
}
