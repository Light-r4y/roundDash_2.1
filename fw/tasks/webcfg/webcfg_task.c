#include "webcfg_task.h"
#include <stdatomic.h>
#include "conf.h"
#include "settings.h"
#include "auth.h"
#include "ota_page.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "WEBCFG";

static atomic_bool s_start_requested = false;
static atomic_bool s_running = false;

// TODO: подъём точки доступа:
//  - esp_netif + esp_wifi в режиме AP: SSID из настроек или RG_WIFI_AP_SSID_PREFIX +
//    хвост MAC, пароль из настроек (по умолчанию RG_WIFI_AP_PASS_DEFAULT), WPA2;
//  - смонтировать раздел www (RG_WWW_PARTITION -> RG_WWW_BASE_PATH);
//  - httpd_start() с RG_HTTPD_STACK/PRIORITY/CORE и обработчиками:
//      GET  /*               статика из www (как spiffs_get_handler в wifi-serv)
//      GET  /ota             RG_OTA_PAGE_HTML
//      POST /api/ota/update  прошивка в свободный ota_N (пароль)
//      POST /api/www/update  образ раздела www целиком (пароль)
//      GET  /api/status      состояние для страниц и /ota
//      GET  /api/auth/status, POST /api/auth/verify, пароль - как в wifi-serv
//      настройки, таблица сигналов, загрузка медиа, сниффер CAN - не определены.
static void start_access_point(void)
{
    const roundGauge_settings_t *cfg = roundGauge_settings_get();
    ESP_LOGW(TAG, "Access point requested (ssid \"%s\") - not implemented yet",
             cfg->wifi_ap.ssid[0] ? cfg->wifi_ap.ssid : RG_WIFI_AP_SSID_PREFIX "XXXX");
}

static void webcfg_task(void *arg)
{
    ESP_LOGI(TAG, "Web config task started, access point is off");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RG_WEBCFG_TICK_MS));

        if (atomic_exchange(&s_start_requested, false) && !atomic_load(&s_running)) {
            atomic_store(&s_running, true);
            start_access_point();
        }
    }
}

void roundGauge_webcfg_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(webcfg_task, "webcfg_task", RG_WEBCFG_TASK_STACK, NULL,
                                            RG_WEBCFG_TASK_PRIORITY, NULL, RG_WEBCFG_TASK_CORE);
    configASSERT(ok == pdPASS);
}

void roundGauge_webcfg_start(void)
{
    atomic_store(&s_start_requested, true);
}

bool roundGauge_webcfg_running(void)
{
    return atomic_load(&s_running);
}
