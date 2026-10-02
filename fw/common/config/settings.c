#include "settings.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "SETTINGS";

static roundGauge_settings_t s_cfg;

static const char *const CAN_MODE_NAMES[] = {
    [RG_CAN_MODE_LISTEN_ONLY] = "listen_only",
    [RG_CAN_MODE_NORMAL] = "normal",
};

void roundGauge_settings_defaults(roundGauge_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = RG_SETTINGS_VERSION;
    out->can.bitrate = RG_CAN_BITRATE_DEFAULT;
    out->can.mode = RG_CAN_LISTEN_ONLY_DEFAULT ? RG_CAN_MODE_LISTEN_ONLY : RG_CAN_MODE_NORMAL;
    out->wifi_ap.ssid[0] = '\0'; // пусто - roundGauge-XXXX по MAC
    strlcpy(out->wifi_ap.password, RG_WIFI_AP_PASS_DEFAULT, sizeof(out->wifi_ap.password));
}

// ------------------------------------------------------------------
// JSON -> структура. Отсутствующее поле оставляет значение по умолчанию.
// ------------------------------------------------------------------

static void get_u32(const cJSON *obj, const char *key, uint32_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsNumber(v) && v->valuedouble >= 0) {
        *out = (uint32_t)v->valuedouble;
    }
}

static void get_str(const cJSON *obj, const char *key, char *out, size_t size)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(v) && v->valuestring != NULL) {
        strlcpy(out, v->valuestring, size);
    }
}

static void get_can_mode(const cJSON *obj, const char *key, roundGauge_can_mode_t *out)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(v) || v->valuestring == NULL) {
        return;
    }
    for (int i = 0; i < (int)(sizeof(CAN_MODE_NAMES) / sizeof(CAN_MODE_NAMES[0])); i++) {
        if (strcmp(v->valuestring, CAN_MODE_NAMES[i]) == 0) {
            *out = (roundGauge_can_mode_t)i;
            return;
        }
    }
    ESP_LOGW(TAG, "can.mode \"%s\" unknown, keeping default", v->valuestring);
}

static bool parse(const char *json, roundGauge_settings_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        return false;
    }

    get_u32(root, "version", &out->version);

    const cJSON *can = cJSON_GetObjectItemCaseSensitive(root, "can");
    get_u32(can, "bitrate", &out->can.bitrate);
    get_can_mode(can, "mode", &out->can.mode);

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(root, "wifi_ap");
    get_str(ap, "ssid", out->wifi_ap.ssid, sizeof(out->wifi_ap.ssid));
    get_str(ap, "password", out->wifi_ap.password, sizeof(out->wifi_ap.password));

    // TODO: валидация (допустимые скорости CAN, длина пароля AP 8-64).

    cJSON_Delete(root);
    return true;
}

static char *serialize(const roundGauge_settings_t *cfg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "version", cfg->version);

    cJSON *can = cJSON_AddObjectToObject(root, "can");
    cJSON_AddNumberToObject(can, "bitrate", cfg->can.bitrate);
    cJSON_AddStringToObject(can, "mode", CAN_MODE_NAMES[cfg->can.mode]);

    cJSON *ap = cJSON_AddObjectToObject(root, "wifi_ap");
    cJSON_AddStringToObject(ap, "ssid", cfg->wifi_ap.ssid);
    cJSON_AddStringToObject(ap, "password", cfg->wifi_ap.password);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

// ------------------------------------------------------------------

void roundGauge_settings_init(void)
{
    roundGauge_settings_defaults(&s_cfg);

    nvs_handle_t handle;
    if (nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "No settings in NVS, using defaults");
        return;
    }

    size_t len = 0;
    esp_err_t err = nvs_get_blob(handle, RG_SETTINGS_NVS_KEY, NULL, &len);
    char *json = (err == ESP_OK && len > 0) ? malloc(len + 1) : NULL;
    if (json != NULL) {
        err = nvs_get_blob(handle, RG_SETTINGS_NVS_KEY, json, &len);
        json[len] = '\0';
    }
    nvs_close(handle);

    if (json == NULL || err != ESP_OK) {
        ESP_LOGW(TAG, "No settings in NVS, using defaults");
    } else if (!parse(json, &s_cfg)) {
        ESP_LOGE(TAG, "Settings JSON is corrupt, using defaults");
        roundGauge_settings_defaults(&s_cfg);
    } else {
        ESP_LOGI(TAG, "Settings loaded from NVS");
    }
    free(json);
}

const roundGauge_settings_t *roundGauge_settings_get(void)
{
    return &s_cfg;
}

esp_err_t roundGauge_settings_save(const roundGauge_settings_t *cfg)
{
    char *json = serialize(cfg);
    if (json == NULL) {
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(RG_SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, RG_SETTINGS_NVS_KEY, json, strlen(json));
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    cJSON_free(json);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save settings: %s", esp_err_to_name(err));
    }
    return err;
}
