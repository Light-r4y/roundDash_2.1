#include "sound_task.h"
#include <string.h>
#include "conf.h"
#include "board.h"
#include "settings.h"
#include "signals.h"
#include "sound_logic.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "SOUND";

static volatile bool s_mute_req;
static volatile bool s_click_req;
static volatile bool s_test_req;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static roundGauge_sound_status_t s_status;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void roundGauge_sound_user_input(void)
{
    s_mute_req = true;
    s_click_req = true;
}

void roundGauge_sound_test(void)
{
    s_test_req = true;
}

void roundGauge_sound_get_status(roundGauge_sound_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static void sound_task(void *arg)
{
    roundGauge_alerts_t rules;
    roundGauge_alert_state_t st[RG_ALERTS_MAX];
    int sig_id[RG_ALERTS_MAX];
    memset(st, 0, sizeof(st));
    uint32_t generation = 0;
    bool have_rules = false;
    bool buzzer = false;
    uint32_t click_until = 0, test_until = 0;
    bool click_on = false, test_on = false;

    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(RG_SOUND_TICK_MS));
        const uint32_t now = now_ms();

        if (!have_rules || generation != roundGauge_alerts_generation()) {
            generation = roundGauge_alerts_generation();
            roundGauge_alerts_copy(&rules);
            memset(st, 0, sizeof(st));
            for (int i = 0; i < rules.count; i++) {
                sig_id[i] = roundGauge_signal_id(rules.r[i].signal);
            }
            have_rules = true;
        }

        roundGauge_sound_settings_t cfg;
        roundGauge_settings_get_sound(&cfg);
        for (int i = 0; i < rules.count; i++) {
            if (sig_id[i] < 0) {
                sig_id[i] = roundGauge_signal_id(rules.r[i].signal); // слот мог освободиться после смены раскладки
            }
        }

        if (s_test_req) {
            s_test_req = false;
            test_until = now + RG_SOUND_TEST_MS;
            test_on = true;
        }
        if (s_click_req) {
            s_click_req = false;
            if (cfg.mode == RG_SOUND_ALERTS_CLICKS) {
                click_until = now + RG_SOUND_CLICK_MS;
                click_on = true;
            }
        }
        const bool mute = s_mute_req;
        s_mute_req = false;
        if (test_on && (int32_t)(now - test_until) >= 0) test_on = false;
        if (click_on && (int32_t)(now - click_until) >= 0) click_on = false;

        bool out = test_on || click_on;
        uint8_t active = 0, muted = 0;
        for (int i = 0; i < rules.count; i++) {
            float v = 0;
            const bool valid = cfg.mode != RG_SOUND_OFF && sig_id[i] >= 0 && roundGauge_signal_get(sig_id[i], &v);
            roundGauge_alert_update(&rules.r[i], &st[i], valid, v, now);
            if (mute && st[i].active) {
                st[i].muted = true;
            }
            if (st[i].active) {
                active |= (uint8_t)(1U << i);
            }
            if (st[i].muted) {
                muted |= (uint8_t)(1U << i);
            }
            if (cfg.mode != RG_SOUND_OFF && roundGauge_alert_output(&rules.r[i], &st[i], now)) {
                out = true;
            }
        }

        if (out != buzzer) {
            if (roundGauge_board_buzzer_set(out) == ESP_OK) {
                buzzer = out;
            }
        }
        portENTER_CRITICAL(&s_lock);
        s_status.active = active;
        s_status.muted = muted;
        s_status.buzzer_on = buzzer;
        portEXIT_CRITICAL(&s_lock);
    }
}

void roundGauge_sound_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(sound_task, "sound_task", RG_SOUND_TASK_STACK, NULL,
                                            RG_SOUND_TASK_PRIORITY, NULL, RG_SOUND_TASK_CORE);
    configASSERT(ok == pdPASS);
    ESP_LOGI(TAG, "Sound task started");
}
