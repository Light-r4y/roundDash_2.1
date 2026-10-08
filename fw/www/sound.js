// Страница "Звук": режим зуммера, пробный писк и правила тревог (tasks/sound, common/config/alerts.h).
const RG_SOUND_MODES = ['off', 'alerts', 'alerts_clicks'];
const RG_ALERTS_MAX = 8;

function rgNormRule(raw) {
    const n = (v, d) => (typeof v === 'number' && !Number.isNaN(v) ? v : d);
    return {
        signal: typeof raw.signal === 'string' ? raw.signal : '',
        op: raw.op === '<' ? '<' : '>',
        value: n(raw.value, 0),
        hyst: n(raw.hyst, 0),
        pattern: raw.pattern === 'continuous' ? 'continuous' : 'beeps',
        count: Math.min(9, Math.max(1, Math.round(n(raw.count, 3)))),
        on: Math.min(2000, Math.max(20, Math.round(n(raw.on, 150)))),
        off: Math.min(2000, Math.max(20, Math.round(n(raw.off, 150)))),
        repeat: Math.min(3600, Math.max(0, Math.round(n(raw.repeat, 5)))),
        enabled: raw.enabled !== false,
    };
}

function soundPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        modes: RG_SOUND_MODES,
        mode: 'off',
        status: { active: 0, muted: 0, buzzer: false },
        rules: [],
        signals: [],
        dirty: false,
        busy: false,
        message: '',
        messageType: 'success',
        maxRules: RG_ALERTS_MAX,
        timer: null,

        async init() {
            this.checkAuthStatus();
            await Promise.all([this.loadRules(), this.loadSignals(), this.pollStatus(true)]);
            this.timer = setInterval(() => this.pollStatus(false), 1000);
        },

        controlsLocked() { return this.authRequired && !this.authUnlocked; },
        say(text, type) { this.message = text; this.messageType = type; },
        changed() { this.dirty = true; },
        num(obj, key, ev) {
            const v = parseFloat(ev.target.value);
            if (!Number.isNaN(v)) { obj[key] = v; this.changed(); }
        },

        async pollStatus(withMode) {
            if (document.hidden && !withMode) return;
            try {
                const res = await fetch('/api/sound', { cache: 'no-store' });
                if (!res.ok) return;
                const d = await res.json();
                if (withMode || !this.busy) this.mode = RG_SOUND_MODES.includes(d.mode) ? d.mode : 'off';
                this.status = { active: d.active | 0, muted: d.muted | 0, buzzer: !!d.buzzer };
            } catch (e) { /* плата занята - следующий опрос */ }
        },

        async loadSignals() {
            try {
                const res = await fetch('/api/layout', { cache: 'no-store' });
                if (!res.ok) return;
                const d = await res.json();
                this.signals = (Array.isArray(d.signals) ? d.signals.map(s => s.name) : []).filter(Boolean);
            } catch (e) { /* без списка сигнал вводится вручную */ }
        },

        async loadRules() {
            try {
                const res = await fetch('/api/sound/rules', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const d = await res.json();
                this.rules = (d.rules || []).slice(0, RG_ALERTS_MAX).map(rgNormRule);
                this.dirty = false;
            } catch (e) {
                this.say(this.t('can_err_load') + ': ' + e.message, 'danger');
            }
        },

        // ---- режим и проверка ----
        async setMode(m) {
            if (this.busy || m === this.mode) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/sound', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ mode: m }),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.mode = m;
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            } finally { this.busy = false; }
        },
        async testBeep() {
            try {
                const res = await this.authFetch('/api/sound/test', { method: 'POST' });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            }
        },

        // ---- правила ----
        get rulesInvalid() {
            return this.rules.some(r => !r.signal || Number.isNaN(r.value));
        },
        addRule() {
            if (this.rules.length >= RG_ALERTS_MAX) return;
            this.rules.push(rgNormRule({ signal: this.signals[0] || '' }));
            this.changed();
        },
        removeRule(i) {
            this.rules.splice(i, 1);
            this.changed();
        },
        async applyRules() {
            if (this.busy || this.rulesInvalid) return;
            this.busy = true;
            try {
                const body = JSON.stringify({ version: 1, rules: this.rules.map(r => ({
                    signal: r.signal, op: r.op, value: r.value, hyst: r.hyst, pattern: r.pattern,
                    count: r.count, on: r.on, off: r.off, repeat: r.repeat, enabled: r.enabled })) });
                const res = await this.authFetch('/api/sound/rules', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' }, body,
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.dirty = false;
                this.say(this.t('sound_applied'), 'success');
            } catch (e) {
                this.say(this.t('can_err_apply') + ': ' + e.message, 'danger');
            } finally { this.busy = false; }
        },
        async resetRules() {
            if (this.busy || !confirm(this.t('sound_defaults_confirm'))) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/sound/rules/reset', { method: 'POST' });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                await this.loadRules();
                this.say(this.t('sound_applied'), 'success');
            } catch (e) {
                this.say(this.t('can_err_apply') + ': ' + e.message, 'danger');
            } finally { this.busy = false; }
        },

        // ---- состояние ----
        alarmNow() { return this.status.active !== 0; },
        statusText() {
            if (this.mode === 'off') return this.t('sound_status_off');
            const names = this.rules.map((r, i) => (this.status.active & (1 << i) ? r.signal : null)).filter(Boolean);
            if (!names.length) return this.t('sound_status_quiet');
            return this.t('sound_status_alarm') + ': ' + names.join(', ');
        },
        ruleState(i) {
            if (!(this.status.active & (1 << i))) return '';
            return this.status.muted & (1 << i) ? this.t('sound_muted') : this.t('sound_active');
        },
        ruleBadgeClass(i) {
            return this.status.muted & (1 << i) ? 'bg-secondary' : 'bg-danger';
        },
        ruleSummary(r) {
            const cond = (this.signalTitle(r.signal) || '-') + ' ' + r.op + ' ' + r.value;
            if (r.pattern === 'continuous') return cond + ' - ' + this.t('sound_pat_cont').toLowerCase();
            const rep = r.repeat > 0 ? ', ' + this.t('sound_each') + ' ' + r.repeat + ' ' + this.t('sound_sec') : ', ' + this.t('sound_once');
            return cond + ' - ' + r.count + '\u00d7 ' + r.on + '/' + r.off + ' ' + this.t('sound_ms') + rep;
        },
        signalTitle(n) { return n; },
    };
}
