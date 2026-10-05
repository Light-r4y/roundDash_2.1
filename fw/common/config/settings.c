#include "settings.h"
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"

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
    out->display.brightness = RG_LCD_BL_DEFAULT_PCT;
    out->imu.g0[2] = 1.0f; // не откалибровано: плата лежит ровно
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
    const cJSON *demo = cJSON_GetObjectItemCaseSensitive(can, "demo");
    if (cJSON_IsBool(demo)) {
        out->can.demo = cJSON_IsTrue(demo);
    }

    const cJSON *disp = cJSON_GetObjectItemCaseSensitive(root, "display");
    uint32_t br = out->display.brightness;
    get_u32(disp, "brightness", &br);
    out->display.brightness = (uint8_t)(br < RG_LCD_BL_MIN_PCT ? RG_LCD_BL_MIN_PCT : (br > 100 ? 100 : br));

    const cJSON *imu = cJSON_GetObjectItemCaseSensitive(root, "imu");
    const cJSON *cal = cJSON_GetObjectItemCaseSensitive(imu, "calibrated");
    if (cJSON_IsBool(cal)) {
        out->imu.calibrated = cJSON_IsTrue(cal);
    }
    const cJSON *g0 = cJSON_GetObjectItemCaseSensitive(imu, "g0");
    if (cJSON_IsArray(g0) && cJSON_GetArraySize(g0) == 3) {
        for (int i = 0; i < 3; i++) {
            const cJSON *v = cJSON_GetArrayItem(g0, i);
            if (cJSON_IsNumber(v)) {
                out->imu.g0[i] = (float)v->valuedouble;
            }
        }
    }
    uint32_t fwd = out->imu.fwd;
    get_u32(imu, "fwd", &fwd);
    out->imu.fwd = (uint8_t)(fwd > 3 ? 0 : fwd);

    const cJSON *ap = cJSON_GetObjectItemCaseSensitive(root, "wifi_ap");
    get_str(ap, "ssid", out->wifi_ap.ssid, sizeof(out->wifi_ap.ssid));
    get_str(ap, "password", out->wifi_ap.password, sizeof(out->wifi_ap.password));

    // TODO: валидация при разборе NVS (скорость CAN, длина пароля AP 8-64); из веба
    // скорость CAN проверяет POST /api/can.

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
    cJSON_AddBoolToObject(can, "demo", cfg->can.demo);

    cJSON *disp = cJSON_AddObjectToObject(root, "display");
    cJSON_AddNumberToObject(disp, "brightness", cfg->display.brightness);

    cJSON *imu = cJSON_AddObjectToObject(root, "imu");
    cJSON_AddBoolToObject(imu, "calibrated", cfg->imu.calibrated);
    cJSON_AddItemToObject(imu, "g0", cJSON_CreateFloatArray(cfg->imu.g0, 3));
    cJSON_AddNumberToObject(imu, "fwd", cfg->imu.fwd);

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

static portMUX_TYPE s_can_lock = portMUX_INITIALIZER_UNLOCKED;

void roundGauge_settings_get_can(roundGauge_can_settings_t *out)
{
    portENTER_CRITICAL(&s_can_lock);
    *out = s_cfg.can;
    portEXIT_CRITICAL(&s_can_lock);
}

void roundGauge_settings_get_wifi(roundGauge_wifi_ap_settings_t *out)
{
    portENTER_CRITICAL(&s_can_lock);
    *out = s_cfg.wifi_ap;
    portEXIT_CRITICAL(&s_can_lock);
}

esp_err_t roundGauge_settings_set_wifi(const roundGauge_wifi_ap_settings_t *w)
{
    roundGauge_settings_t next = s_cfg;
    next.wifi_ap = *w;
    esp_err_t err = roundGauge_settings_save(&next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_can_lock);
        s_cfg.wifi_ap = *w;
        portEXIT_CRITICAL(&s_can_lock);
    }
    return err;
}

esp_err_t roundGauge_settings_reset_wifi(void)
{
    roundGauge_wifi_ap_settings_t w = {0};
    strlcpy(w.password, RG_WIFI_AP_PASS_DEFAULT, sizeof(w.password));
    return roundGauge_settings_set_wifi(&w);
}

void roundGauge_settings_get_imu(roundGauge_imu_settings_t *out)
{
    portENTER_CRITICAL(&s_can_lock);
    *out = s_cfg.imu;
    portEXIT_CRITICAL(&s_can_lock);
}

esp_err_t roundGauge_settings_set_imu(const roundGauge_imu_settings_t *imu)
{
    roundGauge_settings_t next = s_cfg;
    next.imu = *imu;
    esp_err_t err = roundGauge_settings_save(&next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_can_lock);
        s_cfg.imu = *imu;
        portEXIT_CRITICAL(&s_can_lock);
    }
    return err;
}

void roundGauge_settings_get_display(roundGauge_display_settings_t *out)
{
    portENTER_CRITICAL(&s_can_lock);
    *out = s_cfg.display;
    portEXIT_CRITICAL(&s_can_lock);
}

esp_err_t roundGauge_settings_set_display(const roundGauge_display_settings_t *d)
{
    roundGauge_settings_t next = s_cfg;
    next.display = *d;
    esp_err_t err = roundGauge_settings_save(&next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_can_lock);
        s_cfg.display = *d;
        portEXIT_CRITICAL(&s_can_lock);
    }
    return err;
}

esp_err_t roundGauge_settings_set_can(const roundGauge_can_settings_t *can)
{
    roundGauge_settings_t next = s_cfg;
    next.can = *can;
    esp_err_t err = roundGauge_settings_save(&next);
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_can_lock);
        s_cfg.can = *can;
        portEXIT_CRITICAL(&s_can_lock);
    }
    return err;
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
