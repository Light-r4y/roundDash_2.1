#include "signals.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"

typedef struct {
    char name[RG_SIGNAL_NAME_MAX];
    volatile float value;
    volatile bool valid;
    volatile uint32_t stamp_ms; // когда записано
    uint32_t timeout_ms;
    float min, max;
    bool sim_exempt; // реальный источник (датчик): демо-генератор его не трогает
} signal_t;

static signal_t s_sig[RG_SIGNAL_MAX_COUNT];
static int s_count;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_sim;

static inline uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

// create: заводить ли новый сигнал; update_range: перезаписывать ли диапазон найденного.
static int find_or_add(const char *name, float min, float max, bool update_range)
{
    if (name == NULL || name[0] == '\0') {
        return -1;
    }
    int id = -1;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < s_count; i++) {
        if (strncmp(s_sig[i].name, name, RG_SIGNAL_NAME_MAX - 1) == 0) {
            id = i;
            if (update_range) {
                s_sig[i].min = min; // диапазон из свежего layout
                s_sig[i].max = max;
            }
            break;
        }
    }
    if (id < 0 && s_count < RG_SIGNAL_MAX_COUNT) {
        id = s_count++;
        strlcpy(s_sig[id].name, name, sizeof(s_sig[id].name));
        s_sig[id].min = min;
        s_sig[id].max = max;
    }
    portEXIT_CRITICAL(&s_lock);
    return id;
}

int roundGauge_signal_register(const char *name, float min, float max)
{
    return find_or_add(name, min, max, true);
}

int roundGauge_signal_id(const char *name)
{
    return find_or_add(name, 0, 100, false);
}

void roundGauge_signal_set_timeout(int id, uint32_t timeout_ms)
{
    if (id >= 0 && id < RG_SIGNAL_MAX_COUNT) {
        s_sig[id].timeout_ms = timeout_ms;
    }
}

void roundGauge_signal_set(int id, float value)
{
    if (id < 0 || id >= RG_SIGNAL_MAX_COUNT) {
        return;
    }
    s_sig[id].value = value;
    s_sig[id].stamp_ms = now_ms();
    s_sig[id].valid = true;
}

bool roundGauge_signal_get(int id, float *out)
{
    if (id < 0 || id >= RG_SIGNAL_MAX_COUNT || !s_sig[id].valid) {
        return false;
    }
    uint32_t to = s_sig[id].timeout_ms;
    if (to != 0 && (uint32_t)(now_ms() - s_sig[id].stamp_ms) > to) {
        return false;
    }
    *out = s_sig[id].value;
    return true;
}

void roundGauge_signal_invalidate(int id)
{
    if (id >= 0 && id < RG_SIGNAL_MAX_COUNT) {
        s_sig[id].valid = false;
    }
}

void roundGauge_signal_invalidate_all(void)
{
    for (int i = 0; i < RG_SIGNAL_MAX_COUNT; i++) {
        s_sig[i].valid = false;
    }
}

void roundGauge_signal_sim_exempt(int id)
{
    if (id >= 0 && id < RG_SIGNAL_MAX_COUNT) {
        s_sig[id].sim_exempt = true;
    }
}

void roundGauge_signal_sim_enable(bool enable)
{
    bool was = s_sim;
    s_sim = enable;
    if (was && !enable) {
        roundGauge_signal_invalidate_all();
    }
}

bool roundGauge_signal_sim_is_enabled(void)
{
    return s_sim;
}

void roundGauge_signal_sim_step(uint32_t t_ms)
{
    if (!s_sim) {
        return;
    }
    uint32_t stamp = now_ms();
    for (int i = 0; i < s_count; i++) {
        if (s_sig[i].sim_exempt) {
            continue;
        }
        // Период 5-8 с, у каждого сигнала свой, чтобы не двигались в такт.
        uint32_t period = 5000 + (uint32_t)(i % 4) * 1000;
        uint32_t t = (t_ms + (uint32_t)i * 777) % period;
        float x = (float)t / (float)period;           // 0..1
        float tri = x < 0.5f ? x * 2.0f : (1.0f - x) * 2.0f;
        s_sig[i].value = s_sig[i].min + (s_sig[i].max - s_sig[i].min) * tri;
        s_sig[i].stamp_ms = stamp;
        s_sig[i].valid = true;
    }
}
