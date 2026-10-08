#include "sound_logic.h"

void roundGauge_alert_update(const roundGauge_alert_rule_t *r, roundGauge_alert_state_t *st, bool valid, float v,
                             uint32_t now_ms)
{
    if (!r->enabled || !valid) {
        st->active = false;
        st->muted = false;
        return;
    }
    const bool above = r->op == '>';
    const bool trigger = above ? (v > r->value) : (v < r->value);
    const bool release = above ? (v <= r->value - r->hyst) : (v >= r->value + r->hyst);
    if (!st->active) {
        if (trigger) {
            st->active = true;
            st->muted = false;
            st->t_start_ms = now_ms;
        }
    } else if (release) {
        st->active = false;
        st->muted = false;
    }
}

bool roundGauge_alert_output(const roundGauge_alert_rule_t *r, const roundGauge_alert_state_t *st, uint32_t now_ms)
{
    if (!st->active || st->muted) {
        return false;
    }
    if (r->pattern == RG_ALERT_CONTINUOUS) {
        return true;
    }
    const uint32_t unit = (uint32_t)r->on_ms + r->off_ms;
    if (unit == 0) {
        return false;
    }
    const uint32_t series = (uint32_t)r->count * unit;
    const uint32_t dt = now_ms - st->t_start_ms;
    uint32_t pos;
    if (r->repeat_s == 0) {
        if (dt >= series) {
            return false;
        }
        pos = dt;
    } else {
        uint32_t cycle = (uint32_t)r->repeat_s * 1000U;
        if (cycle < series) {
            cycle = series;
        }
        pos = dt % cycle;
        if (pos >= series) {
            return false;
        }
    }
    return (pos % unit) < r->on_ms;
}
