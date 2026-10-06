// Страница CAN: настройки шины, таблица привязок сигналов к кадрам и сниффер.
// Правила проверки привязок здесь должны совпадать с can_map.c (parse_entry).
const RG_CAN_MAP_MAX = 16;
const RG_CAN_BITRATES = [50000, 100000, 125000, 250000, 500000, 1000000];
const RG_CAN_STATES = ['active', 'warning', 'passive', 'bus_off'];

// Влезает ли поле целиком в 8 байт - как roundGauge_can_extract() в прошивке.
function rgCanFits(start, len, order) {
    if (!Number.isInteger(start) || !Number.isInteger(len) || len < 1 || len > 32 || start < 0 || start > 63) return false;
    if (order === 'intel') return start + len <= 64;
    let p = start;
    for (let i = 0; i < len; i++) {
        if (p < 0 || p > 63) return false;
        p = (p & 7) === 0 ? p + 15 : p - 1;
    }
    return true;
}

// "0x201" | "513" | 513 -> число или null.
function rgParseId(v) {
    if (typeof v === 'number') return Number.isInteger(v) && v >= 0 && v <= 0x1FFFFFFF ? v : null;
    const s = String(v ?? '').trim();
    if (!/^(0x[0-9a-fA-F]+|[0-9]+)$/.test(s)) return null;
    const n = Number(s);
    return Number.isInteger(n) && n >= 0 && n <= 0x1FFFFFFF ? n : null;
}

const rgHex = (n, ext) => '0x' + n.toString(16).toUpperCase().padStart(ext ? 8 : 3, '0');

function rgNewEntry(signal) {
    return { signal, id: '0x100', ext: false, start: 0, len: 8, order: 'intel', signed: false,
             factor: 1, offset: 0, timeout: 2000 };
}

function rgNormEntry(raw) {
    const e = Object.assign(rgNewEntry(''), raw);
    const id = rgParseId(raw.id);
    e.id = id === null ? String(raw.id ?? '') : rgHex(id, !!raw.ext);
    e.ext = !!raw.ext;
    e.signed = !!raw.signed;
    e.order = raw.order === 'motorola' ? 'motorola' : 'intel';
    return e;
}

function canPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        // настройки и состояние шины
        cfg: { bitrate: 500000, mode: 'listen_only', demo: false },
        status: null,
        rxRate: 0,
        lastRx: null,

        // таблица привязок
        map: [],
        signals: [],
        mapDirty: false,
        cfgDirty: false,

        // сниффер
        frames: [],
        prev: {},        // ключ -> { data, count, t }
        changed: {},     // ключ -> { байт: время последнего изменения }
        paused: false,
        filter: '',

        busy: false,
        message: '',
        messageType: 'success',
        timers: [],

        init() {
            this.checkAuthStatus();
            this.loadAll();
            this.timers.push(setInterval(() => this.pollStatus(), 1000));
            this.timers.push(setInterval(() => this.pollFrames(), 500));
        },

        get bitrateOptions() {
            return RG_CAN_BITRATES.includes(this.cfg.bitrate) ? RG_CAN_BITRATES : [...RG_CAN_BITRATES, this.cfg.bitrate].sort((a, b) => a - b);
        },

        // ---- загрузка ----
        async loadAll() {
            await Promise.all([this.loadCfg(), this.loadMap(), this.loadSignals()]);
        },

        async loadCfg() {
            try {
                const res = await fetch('/api/can', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const d = await res.json();
                this.cfg = { bitrate: d.bitrate, mode: d.mode, demo: !!d.demo };
                this.status = d;
                this.cfgDirty = false;
            } catch (e) {
                this.say(this.t('can_err_load') + ': ' + e.message, 'danger');
            }
        },

        async loadMap() {
            try {
                const res = await fetch('/api/can/map', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                const d = await res.json();
                this.map = (d.map || []).slice(0, RG_CAN_MAP_MAX).map(rgNormEntry);
                this.mapDirty = false;
            } catch (e) {
                this.say(this.t('can_err_load') + ': ' + e.message, 'danger');
            }
        },

        // Вернуть таблицу к встроенному пресету rusEFI (плата стирает сохранённую и берёт свою).
        async resetMapPreset() {
            if (this.busy || !confirm(this.t('can_preset_confirm'))) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/can/map/reset', { method: 'POST' });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                await this.loadMap();
                this.say(this.t('can_preset_done'), 'success');
            } catch (e) {
                this.say(this.t('can_err_apply') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        // Имена сигналов - из таблицы сигналов layout (версия 2) или, у старого layout, с экранов.
        async loadSignals() {
            try {
                const res = await fetch('/api/layout', { cache: 'no-store' });
                if (!res.ok) return;
                const d = await res.json();
                const names = Array.isArray(d.signals) ? d.signals.map(s => s.name)
                    : [...new Set((d.screens || []).map(s => s.signal))];
                this.signals = names.filter(Boolean);
            } catch (e) { /* без списка имён сигнал вводится вручную */ }
        },

        // ---- состояние шины ----
        async pollStatus() {
            if (document.hidden) return;
            try {
                const res = await fetch('/api/can', { cache: 'no-store' });
                if (!res.ok) return;
                const d = await res.json();
                const now = performance.now();
                if (this.lastRx) {
                    this.rxRate = Math.max(0, Math.round((d.rx - this.lastRx.rx) * 1000 / (now - this.lastRx.t)));
                }
                this.lastRx = { rx: d.rx, t: now };
                this.status = d;
            } catch (e) { /* плата занята - следующий опрос */ }
        },

        stateLabel() {
            const s = this.status;
            if (!s) return '...';
            if (!s.node_up) return this.t('can_state_down');
            return this.t('can_state_' + (RG_CAN_STATES[s.state] || 'active'));
        },
        stateClass() {
            const s = this.status;
            if (!s || !s.node_up || s.state >= 3) return 'bg-danger';
            return s.state === 0 ? 'bg-success' : 'bg-warning text-dark';
        },

        // ---- настройки ----
        cfgChanged() { this.cfgDirty = true; },

        async applyCfg() {
            if (this.busy) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/can', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ bitrate: Number(this.cfg.bitrate), mode: this.cfg.mode, demo: this.cfg.demo }),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.cfgDirty = false;
                this.say(this.t('can_cfg_applied'), 'success');
                setTimeout(() => this.pollStatus(), 400);
            } catch (e) {
                this.say(this.t('can_err_apply') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        // ---- таблица привязок ----
        mapChanged() { this.mapDirty = true; this.message = ''; },

        addEntry() {
            if (this.map.length >= RG_CAN_MAP_MAX) return;
            const used = new Set(this.map.map(e => e.signal));
            this.map.push(rgNewEntry(this.signals.find(n => !used.has(n)) || ''));
            this.mapChanged();
        },

        removeEntry(i) {
            this.map.splice(i, 1);
            this.mapChanged();
        },

        // Текст ошибки для строки или '' - как правила parse_entry в прошивке.
        entryError(e, i) {
            if (!e.signal) return this.t('can_e_signal');
            if (this.map.some((x, j) => j !== i && x.signal === e.signal)) return this.t('can_e_dup');
            const id = rgParseId(e.id);
            if (id === null) return this.t('can_e_id');
            if (!e.ext && id > 0x7FF) return this.t('can_e_ext');
            if (!rgCanFits(Number(e.start), Number(e.len), e.order)) return this.t('can_e_fit');
            if (!(Number(e.timeout) >= 100 && Number(e.timeout) <= 60000)) return this.t('can_e_timeout');
            return '';
        },

        get mapInvalid() { return this.map.some((e, i) => this.entryError(e, i)); },

        entryNumber(e, key, ev) {
            const v = parseFloat(ev.target.value);
            if (!Number.isNaN(v)) { e[key] = v; this.mapChanged(); }
        },

        mapJson() {
            return JSON.stringify({
                version: 1,
                map: this.map.map(e => {
                    const o = { signal: e.signal, id: rgHex(rgParseId(e.id), e.ext), start: Number(e.start), len: Number(e.len) };
                    if (e.ext) o.ext = true;
                    if (e.order === 'motorola') o.order = 'motorola';
                    if (e.signed) o.signed = true;
                    if (Number(e.factor) !== 1) o.factor = Number(e.factor);
                    if (Number(e.offset) !== 0) o.offset = Number(e.offset);
                    if (Number(e.timeout) !== 2000) o.timeout = Number(e.timeout);
                    return o;
                }),
            });
        },

        async applyMap() {
            if (this.busy || this.mapInvalid) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/can/map', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: this.mapJson(),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.mapDirty = false;
                this.say(this.t('can_map_applied'), 'success');
            } catch (e) {
                this.say(this.t('can_err_apply') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        async reloadMap() {
            if (this.mapDirty && !confirm(this.t('ed_confirm_discard'))) return;
            await this.loadMap();
        },

        // ---- сниффер ----
        key(f) { return (f.ext ? 'x' : 's') + f.id; },

        mapped(f) {
            return this.map.some(e => rgParseId(e.id) === f.id && !!e.ext === !!f.ext);
        },

        async pollFrames() {
            if (this.paused || document.hidden) return;
            try {
                const res = await fetch('/api/can/frames', { cache: 'no-store' });
                if (!res.ok) return;
                const list = await res.json();
                const now = performance.now();
                const out = [];
                const nextPrev = {};
                for (const f of list) {
                    const k = this.key(f);
                    const p = this.prev[k];
                    const bytes = f.data.match(/../g) || [];
                    // Период кадра = время окна / число кадров в нём. Окно растёт, пока кадров нет,
                    // и сдвигается, когда накопилось >= 0,5 с и хотя бы один кадр; нет кадров 3 с - прочерк.
                    let period = p ? p.period : null, aCount = p ? p.aCount : f.count, aT = p ? p.aT : now;
                    if (p) {
                        const dt = now - aT, dc = f.count - aCount;
                        if (dc > 0 && dt >= 500) { period = dt / dc; aCount = f.count; aT = now; }
                        else if (dc === 0 && dt >= 3000) { period = null; aT = now; }
                    }
                    if (p) {
                        const ch = this.changed[k] || (this.changed[k] = {});
                        bytes.forEach((b, i) => { if (p.bytes[i] !== b) ch[i] = now; });
                    }
                    nextPrev[k] = { bytes, count: f.count, t: now, period, aCount, aT };
                    out.push({ ...f, k, bytes, period });
                }
                this.prev = nextPrev;
                out.sort((a, b) => (a.ext - b.ext) || (a.id - b.id));
                this.frames = out;
            } catch (e) { /* плата занята - следующий опрос */ }
        },

        // Период кадра, мс: до 100 - с десятыми, дальше целые; нет данных - прочерк.
        periodText(f) {
            if (f.period == null) return '-';
            return (f.period < 100 ? f.period.toFixed(1) : Math.round(f.period)) + ' ' + this.t('can_ms');
        },

        // Байт изменился меньше секунды назад - подсвечиваем.
        hot(f, i) {
            const t = this.changed[f.k] && this.changed[f.k][i];
            return !!t && performance.now() - t < 1000;
        },

        get shownFrames() {
            const q = this.filter.trim().toLowerCase().replace(/^0x/, '');
            return q ? this.frames.filter(f => f.id.toString(16).includes(q)) : this.frames;
        },

        idLabel(f) { return rgHex(f.id, f.ext); },

        say(text, type) {
            this.message = text;
            this.messageType = type;
        },
    };
}
