// Страница "Логи": запись сигналов на microSD (tasks/logger), условия старта, список файлов.
const RG_LOG_MODES = ['manual', 'boot', 'condition'];

function logsPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        modes: RG_LOG_MODES,
        st: { state: 'no_card', error: '', file: '', records: 0, bytes: 0, seconds: 0, total_kb: 0, free_kb: 0 },
        cfg: { mode: 'manual', signal: '', op: '>', value: 500, stop_delay: 10 },
        signals: [],
        files: [],
        filesLoaded: false,
        dirty: false,
        busy: false,
        message: '',
        messageType: 'success',
        timer: null,

        async init() {
            this.checkAuthStatus();
            this.syncClock();
            await Promise.all([this.loadSignals(), this.poll(true)]);
            if (this.st.state !== 'no_card') this.loadFiles();
            this.timer = setInterval(() => this.poll(false), 2000);
        },

        controlsLocked() { return this.authRequired && !this.authUnlocked; },
        say(text, type) { this.message = text; this.messageType = type; },
        changed() { this.dirty = true; },
        num(key, ev) {
            const v = parseFloat(ev.target.value);
            this.cfg[key] = Number.isNaN(v) ? NaN : v;
            this.changed();
        },
        get cfgInvalid() {
            if (Number.isNaN(this.cfg.value) || Number.isNaN(this.cfg.stop_delay)) return true;
            if (this.cfg.stop_delay < 0 || this.cfg.stop_delay > 3600) return true;
            return this.cfg.mode === 'condition' && !this.cfg.signal;
        },

        // У платы нет часов с питанием: время для дат файлов и метки в заголовке лога берётся у браузера.
        async syncClock() {
            try {
                await fetch('/api/log/time', {
                    method: 'POST', headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ unix: Math.floor(Date.now() / 1000) }),
                });
            } catch (e) { /* не критично */ }
        },

        async poll(withCfg) {
            if (document.hidden && !withCfg) return;
            try {
                const res = await fetch('/api/log', { cache: 'no-store' });
                if (!res.ok) return;
                const d = await res.json();
                const was = this.st.state;
                this.st = {
                    state: d.state, error: d.error || '', file: d.file || '', records: d.records | 0, bytes: d.bytes | 0,
                    seconds: d.seconds | 0, total_kb: d.total_kb || 0, free_kb: d.free_kb || 0,
                };
                if (withCfg || !this.dirty) {
                    this.cfg = {
                        mode: RG_LOG_MODES.includes(d.mode) ? d.mode : 'manual', signal: d.signal || '',
                        op: d.op === '<' ? '<' : '>', value: typeof d.value === 'number' ? d.value : 0,
                        stop_delay: typeof d.stop_delay === 'number' ? d.stop_delay : 10,
                    };
                }
                // карта появилась или запись закончилась (в том числе по условию): список изменился
                if (!withCfg && ((was === 'no_card' && this.st.state !== 'no_card') ||
                    (was === 'recording') !== (this.st.state === 'recording'))) {
                    this.loadFiles();
                }
                if (this.st.state === 'no_card') { this.files = []; this.filesLoaded = false; }
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

        async loadFiles() {
            try {
                const res = await fetch('/api/log/files', { cache: 'no-store' });
                if (res.status === 409) { this.files = []; this.filesLoaded = false; return; }
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const d = await res.json();
                this.files = Array.isArray(d.files) ? d.files : [];
                this.filesLoaded = true;
            } catch (e) {
                this.say(this.t('log_err_files') + ': ' + e.message, 'danger');
            }
        },

        // ---- действия ----
        async post(url, body) {
            const opts = { method: 'POST' };
            if (body !== undefined) {
                opts.headers = { 'Content-Type': 'application/json' };
                opts.body = JSON.stringify(body);
            }
            const res = await this.authFetch(url, opts);
            if (res.status === 401) return false;
            if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
            return true;
        },
        async act(fn, errKey) {
            if (this.busy) return;
            this.busy = true;
            try {
                if (await fn()) this.message = '';
            } catch (e) {
                this.say(this.t(errKey) + ': ' + e.message, 'danger');
            } finally { this.busy = false; }
        },
        async startLog() {
            await this.act(async () => {
                if (!await this.post('/api/log/start')) return false;
                await new Promise(r => setTimeout(r, 400));
                await this.poll(false);
                return true;
            }, 'set_err');
        },
        async stopLog() {
            await this.act(async () => {
                if (!await this.post('/api/log/stop')) return false;
                await new Promise(r => setTimeout(r, 400));
                await this.poll(false);
                return true;
            }, 'set_err');
        },
        async rescan() {
            await this.act(async () => {
                if (!await this.post('/api/log/rescan')) return false;
                await new Promise(r => setTimeout(r, 1500));
                await this.poll(false);
                if (this.st.state !== 'no_card') await this.loadFiles();
                return true;
            }, 'set_err');
        },
        setMode(m) {
            if (this.cfg.mode === m) return;
            this.cfg.mode = m;
            if (m === 'condition' && !this.cfg.signal) {
                this.cfg.signal = this.signals.includes('rpm') ? 'rpm' : (this.signals[0] || '');
            }
            this.changed();
        },
        async applyCfg() {
            if (this.cfgInvalid) return;
            await this.act(async () => {
                const body = { mode: this.cfg.mode, signal: this.cfg.signal, op: this.cfg.op,
                    value: this.cfg.value, stop_delay: Math.round(this.cfg.stop_delay) };
                if (!await this.post('/api/log', body)) return false;
                this.dirty = false;
                this.say(this.t('log_applied'), 'success');
                return false;
            }, 'can_err_apply');
        },
        async removeFile(name) {
            if (!confirm(this.t('log_delete_confirm').replace('{name}', name))) return;
            await this.act(async () => {
                if (!await this.post('/api/log/delete?name=' + encodeURIComponent(name))) return false;
                await this.loadFiles();
                await this.poll(false);
                return true;
            }, 'log_err_delete');
        },

        // ---- вывод ----
        isActive(name) { return this.st.state === 'recording' && this.st.file.toLowerCase() === name; },
        stateText() {
            return this.t('log_state_' + this.st.state);
        },
        stateBadge() {
            return { no_card: 'bg-secondary', idle: 'bg-success', recording: 'bg-danger' }[this.st.state] || 'bg-secondary';
        },
        recordingText() {
            const s = this.st.seconds;
            const dur = Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0');
            return this.st.file.toLowerCase() + ' - ' + dur + ', ' + this.fmtBytes(this.st.bytes);
        },
        errorText() {
            return this.st.error ? this.t('log_err_' + this.st.error) : '';
        },
        conditionText() {
            const c = this.cfg;
            if (!c.signal || Number.isNaN(c.value)) return '';
            return this.t('log_cond_text').replace('{cond}', c.signal + ' ' + c.op + ' ' + c.value)
                .replace('{n}', Number.isNaN(c.stop_delay) ? '?' : String(Math.round(c.stop_delay)));
        },
        fmtBytes(n) {
            if (n < 1024) return n + ' B';
            if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
            return (n / 1048576).toFixed(1) + ' MB';
        },
        fmtKb(kb) {
            if (kb < 1024) return kb + ' KB';
            if (kb < 1048576) return (kb / 1024).toFixed(0) + ' MB';
            return (kb / 1048576).toFixed(1) + ' GB';
        },
        // Дата файла; плата без выставленного времени отдаёт 1980-й год - такую не показываем.
        fmtTime(t) {
            if (!t || t < 1700000000) return '';
            return new Date(t * 1000).toLocaleString(this.lang === 'ru' ? 'ru-RU' : 'en-GB',
                { dateStyle: 'short', timeStyle: 'short' });
        },
    };
}
