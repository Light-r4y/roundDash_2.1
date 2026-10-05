// Редактор экранов приборки: правит layout версии 2 (таблица сигналов + экраны с
// дополнительными виджетами, см. common/config/layout.h), показывает предпросмотр на
// canvas с перетаскиванием виджетов и отправляет результат на плату.
// Значения по умолчанию и пределы здесь должны совпадать с layout.c / conf.h.
const RG_LIMITS = { screens: 4, widgets: 4, signals: 16, zones: 4, markers: 4, json: 12288 };
const RG_SWEEP_MS = 5000;
const RG_SEC_KEY = 'rg_editor_sections';
// Основное открыто, тонкие настройки свёрнуты.
const RG_SEC_DEFAULT = { main: true, scale: false, ranges: false, widgets: true, wmore: false, signals: false };
const RG_GRID = 5;      // шаг привязки при перетаскивании, px
const RG_GREY = '#9e9e9e';

const RG_SCREEN_DEFAULTS = {
    type: 'dial', signal: '', has_range: false, min: 0, max: 100,
    bg_color: '#000000', bg_image: '', color: '#ffffff', text_color: '#ffffff',
    angle: 270, rotation: 135, ticks: 41, major_every: 5, label_div: 1,
    needle_color: '#ff8000', needle_width: 6, needle_image: '', ring_width: 36,
};

const RG_WIDGET_DEFAULTS = {
    type: 'value', x: 0, y: 0, w: 0, h: 0, signal: '', font: '', text: '', image: '',
    color: '#ffffff', bg_color: '#303030', decimals: -1, zone_color: false, blink: false,
    op: '>', threshold: 0, angle: 270, rotation: 135, width: 12,
};
// Отличия по типам - как в parse_widget (layout.c).
const RG_WIDGET_TYPE_DEFAULTS = {
    value: { font: '48' },
    text: { font: '28', color: RG_GREY },
    image: {},
    indicator: { w: 40, color: '#ff3030' },
    bar: { w: 200, h: 16, color: '#00c0ff' },
    arc: { w: 120, color: '#00c0ff' },
};
// Какие поля имеют смысл у каждого типа (только они уходят в JSON).
const RG_WIDGET_FIELDS = {
    value: ['x', 'y', 'signal', 'font', 'color', 'decimals', 'zone_color'],
    text: ['x', 'y', 'text', 'font', 'color'],
    image: ['x', 'y', 'image'],
    indicator: ['x', 'y', 'signal', 'w', 'color', 'image', 'op', 'threshold', 'blink'],
    bar: ['x', 'y', 'signal', 'w', 'h', 'color', 'bg_color', 'zone_color'],
    arc: ['x', 'y', 'signal', 'w', 'width', 'angle', 'rotation', 'color', 'bg_color', 'zone_color'],
};
const RG_SCREEN_FIELDS = {
    dial: ['bg_color', 'bg_image', 'color', 'text_color', 'angle', 'rotation', 'ticks', 'major_every', 'label_div',
           'needle_color', 'needle_width', 'needle_image'],
    ring: ['bg_color', 'bg_image', 'color', 'text_color', 'angle', 'rotation', 'ring_width'],
    number: ['bg_color', 'bg_image', 'color', 'text_color'],
};

// Картинки предпросмотра живут вне реактивного состояния Alpine (канвасы не нужно проксировать).
const rgImages = {};
const rgPending = new Set(); // имена, которые уже качаются
let rgValues = {};           // значения сигналов для предпросмотра

const rgWidgetDefault = (type, key) => (RG_WIDGET_TYPE_DEFAULTS[type] && key in RG_WIDGET_TYPE_DEFAULTS[type])
    ? RG_WIDGET_TYPE_DEFAULTS[type][key] : RG_WIDGET_DEFAULTS[key];

// Без ключей со значением undefined: иначе Object.assign затрёт ими умолчания.
const rgClean = (raw) => Object.fromEntries(Object.entries(raw).filter(([, v]) => v !== undefined && v !== null));

function rgNormSignal(raw) {
    raw = rgClean(raw);
    const s = Object.assign({ name: '', title: '', unit: '', min: 0, max: 100, decimals: 0 }, raw);
    s.zones = (raw.zones || []).slice(0, RG_LIMITS.zones).map(z => ({
        from: z.from ?? s.min, to: z.to ?? s.max, color: z.color || '#ffffff' }));
    return s;
}

function rgNormWidget(raw) {
    raw = rgClean(raw);
    const type = RG_WIDGET_FIELDS[raw.type] ? raw.type : 'value';
    return Object.assign({}, RG_WIDGET_DEFAULTS, RG_WIDGET_TYPE_DEFAULTS[type], raw, { type });
}

function rgNormScreen(raw) {
    raw = rgClean(raw);
    const sc = Object.assign({}, RG_SCREEN_DEFAULTS, raw);
    sc.has_range = typeof raw.min === 'number' && typeof raw.max === 'number' && raw.max > raw.min;
    sc.zones = (raw.zones || []).slice(0, RG_LIMITS.zones).map(z => ({
        from: z.from ?? sc.min, to: z.to ?? sc.max, color: z.color || sc.color }));
    sc.markers = (raw.markers || []).slice(0, RG_LIMITS.markers).map(m => ({
        value: m.value ?? sc.min, color: m.color || sc.color }));
    sc.widgets = (raw.widgets || []).slice(0, RG_LIMITS.widgets).map(rgNormWidget);
    return sc;
}

// Текстовые виджеты "как в версии 1": название, значение, единицы на старых местах.
function rgLegacyWidgets(raw, sc) {
    const out = [];
    const yTitle = sc.type === 'ring' ? -70 : -90;
    const yValue = sc.type === 'dial' ? 60 : 0;
    const yUnit = sc.type === 'dial' ? 110 : 60;
    if (raw.title) out.push(rgNormWidget({ type: 'text', text: raw.title, y: yTitle }));
    if (raw.show_value !== false) {
        out.push(rgNormWidget({ type: 'value', y: yValue, color: sc.text_color }));
        if (raw.unit) out.push(rgNormWidget({ type: 'text', text: raw.unit, y: yUnit }));
    }
    return out;
}

// JSON платы (версия 1 или 2) -> модель редактора.
function rgParseLayout(data) {
    const legacy = !Array.isArray(data.signals);
    const layout = { signals: [], screens: [] };
    if (!legacy) {
        layout.signals = data.signals.slice(0, RG_LIMITS.signals).map(rgNormSignal)
            .filter(s => s.name && s.max > s.min);
    }
    for (const raw of (data.screens || []).slice(0, RG_LIMITS.screens)) {
        const sc = rgNormScreen(raw);
        if (!sc.signal) continue;
        if (legacy) {
            if (!layout.signals.some(s => s.name === sc.signal) && layout.signals.length < RG_LIMITS.signals) {
                layout.signals.push(rgNormSignal({ name: sc.signal, title: raw.title, unit: raw.unit,
                    min: sc.min, max: sc.max, decimals: raw.decimals, zones: raw.zones }));
            }
            sc.has_range = false;
            if (!(raw.widgets || []).length) sc.widgets = rgLegacyWidgets(raw, sc);
        }
        layout.screens.push(sc);
    }
    return layout;
}

// Модель -> JSON для платы. В нём только то, что отличается от умолчаний: размер ограничен.
function rgSerialize(layout) {
    const signals = layout.signals.map(s => {
        const o = { name: s.name };
        if (s.title) o.title = s.title;
        if (s.unit) o.unit = s.unit;
        o.min = s.min; o.max = s.max;
        if (s.decimals) o.decimals = s.decimals;
        if (s.zones.length) o.zones = s.zones.map(z => ({ from: z.from, to: z.to, color: z.color }));
        return o;
    });
    const screens = layout.screens.map(sc => {
        const o = { type: sc.type, signal: sc.signal };
        if (sc.has_range) { o.min = sc.min; o.max = sc.max; }
        for (const k of RG_SCREEN_FIELDS[sc.type]) {
            if (sc[k] !== RG_SCREEN_DEFAULTS[k] && sc[k] !== '') o[k] = sc[k];
        }
        if (sc.zones.length) o.zones = sc.zones.map(z => ({ from: z.from, to: z.to, color: z.color }));
        if (sc.type === 'dial' && sc.markers.length) o.markers = sc.markers.map(m => ({ value: m.value, color: m.color }));
        o.widgets = sc.widgets.map(w => {
            const wo = { type: w.type };
            for (const k of RG_WIDGET_FIELDS[w.type]) {
                if (w[k] !== rgWidgetDefault(w.type, k) && w[k] !== '') wo[k] = w[k];
            }
            return wo;
        });
        return o;
    });
    return JSON.stringify({ version: 2, signals, screens });
}

function editorPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        layout: { signals: [], screens: [] },
        sel: 0,            // номер экрана
        selW: -1,          // номер выделенного виджета
        sigOpen: -1,       // сигнал с раскрытыми зонами
        loaded: false,
        dirty: false,
        busy: false,
        message: '',
        messageType: 'success',

        // Предпросмотр
        sweep: true,
        manual: {},
        sweepStart: performance.now(),
        drag: null,

        // Файлы на плате (только список - для выбора картинок и шрифтов; загрузка - на media.html)
        media: { files: [], total: 0, used: 0, max_file: 1000000 },

        // Свёрнутые/развёрнутые секции; состояние помнится в браузере.
        sections: {},

        showJson: false,
        jsonText: '',

        init() {
            try { this.sections = JSON.parse(localStorage.getItem(RG_SEC_KEY) || '{}') || {}; } catch (e) { this.sections = {}; }
            this.checkAuthStatus();
            this.loadAll();
            const loop = (t) => {
                this.frame(t);
                requestAnimationFrame(loop);
            };
            requestAnimationFrame(loop);
        },

        get screen() { return this.layout.screens[this.sel] || null; },
        get widget() { return this.screen && this.screen.widgets[this.selW] || null; },
        get jsonSize() { return new Blob([rgSerialize(this.layout)]).size; },
        get tooBig() { return this.jsonSize > RG_LIMITS.json; },
        get limits() { return RG_LIMITS; },

        // ---- секции ----
        isOpen(k) { return k in this.sections ? !!this.sections[k] : RG_SEC_DEFAULT[k]; },
        toggleSec(k) {
            this.sections = { ...this.sections, [k]: !this.isOpen(k) };
            try { localStorage.setItem(RG_SEC_KEY, JSON.stringify(this.sections)); } catch (e) { /* без памяти браузера работает */ }
        },

        // ---- загрузка ----
        async loadAll() {
            await Promise.all([this.loadLayout(), this.loadMedia()]);
            await this.loadImages();
            this.loaded = true;
        },

        async loadLayout() {
            try {
                const res = await fetch('/api/layout', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                this.layout = rgParseLayout(await res.json());
                this.sel = Math.min(this.sel, Math.max(0, this.layout.screens.length - 1));
                this.selW = -1;
                this.dirty = false;
            } catch (e) {
                this.say(this.t('ed_err_load') + ': ' + e.message, 'danger');
            }
        },

        async loadMedia() {
            try {
                const res = await fetch('/api/media', { cache: 'no-store' });
                if (res.ok) this.media = await res.json();
            } catch (e) { /* список файлов необязателен */ }
        },

        usedImages() {
            const names = new Set();
            for (const sc of this.layout.screens) {
                if (sc.bg_image) names.add(sc.bg_image);
                if (sc.needle_image) names.add(sc.needle_image);
                for (const w of sc.widgets) if (w.image) names.add(w.image);
            }
            return names;
        },

        // Картинки, на которые ссылаются экраны, - в кэш предпросмотра.
        async loadImages() {
            for (const name of this.usedImages()) {
                const f = this.media.files.find(x => x.name === name);
                if (!f || rgPending.has(name) || (rgImages[name] && rgImages[name].size === f.size)) continue;
                rgPending.add(name);
                try {
                    const res = await fetch('/media/' + encodeURIComponent(name), { cache: 'no-store' });
                    const canvas = res.ok ? RgLvBin.decode(await res.arrayBuffer()) : null;
                    if (canvas) { canvas.size = f.size; rgImages[name] = canvas; }
                } catch (e) { /* без картинки рисуем примитивами, как плата */ }
                rgPending.delete(name);
            }
            for (const k of Object.keys(rgImages)) {
                if (!this.media.files.some(x => x.name === k)) delete rgImages[k];
            }
        },

        // ---- предпросмотр ----
        // Все сигналы качаются треугольной волной в своём диапазоне (как генератор
        // в прошивке); без качания - значения с ползунков.
        frame(t) {
            const sc = this.screen;
            const canvas = this.$refs.preview;
            if (!sc || !canvas) return;
            const values = {};
            this.layout.signals.forEach((s, i) => {
                if (this.sweep) {
                    const period = RG_SWEEP_MS + (i % 4) * 1000;
                    const x = (((t - this.sweepStart) + i * 777) % period) / period;
                    values[s.name] = s.min + (s.max - s.min) * (x < 0.5 ? x * 2 : (1 - x) * 2);
                } else {
                    values[s.name] = this.manual[s.name] ?? (s.min + s.max) / 2;
                }
            });
            rgValues = values;
            RgGauge.draw(canvas, this.layout, sc, values, rgImages, this.selW, true);
        },

        // Сигналы, которые использует экран (основной и виджетов) - для ползунков.
        screenSignals() {
            const sc = this.screen;
            if (!sc) return [];
            const names = [sc.signal];
            for (const w of sc.widgets) if (w.signal) names.push(w.signal);
            return [...new Set(names)].map(n => this.layout.signals.find(s => s.name === n)).filter(Boolean);
        },

        sliderValue(s) { return this.manual[s.name] ?? (s.min + s.max) / 2; },
        setManual(s, ev) { this.manual = { ...this.manual, [s.name]: parseFloat(ev.target.value) }; },

        // ---- перетаскивание виджетов на предпросмотре ----
        canvasPoint(ev) {
            const r = this.$refs.preview.getBoundingClientRect();
            return [(ev.clientX - r.left) * RgGauge.SIZE / r.width, (ev.clientY - r.top) * RgGauge.SIZE / r.height];
        },

        dragStart(ev) {
            const sc = this.screen;
            if (!sc) return;
            const [x, y] = this.canvasPoint(ev);
            const hit = RgGauge.hitTest(this.$refs.preview, this.layout, sc, rgValues, rgImages, x, y);
            this.selW = hit;
            if (hit < 0) return;
            const w = sc.widgets[hit];
            this.drag = { i: hit, dx: x - RgGauge.SIZE / 2 - w.x, dy: y - RgGauge.SIZE / 2 - w.y };
            this.$refs.preview.setPointerCapture(ev.pointerId);
            ev.preventDefault();
        },

        dragMove(ev) {
            if (!this.drag) return;
            const w = this.screen.widgets[this.drag.i];
            const [x, y] = this.canvasPoint(ev);
            let nx = x - RgGauge.SIZE / 2 - this.drag.dx, ny = y - RgGauge.SIZE / 2 - this.drag.dy;
            if (!ev.shiftKey) { nx = Math.round(nx / RG_GRID) * RG_GRID; ny = Math.round(ny / RG_GRID) * RG_GRID; }
            const half = RgGauge.SIZE / 2;
            nx = Math.max(-half, Math.min(half, Math.round(nx)));
            ny = Math.max(-half, Math.min(half, Math.round(ny)));
            if (nx !== w.x || ny !== w.y) { w.x = nx; w.y = ny; this.changed(); }
        },

        dragEnd() { this.drag = null; },

        // ---- правка ----
        changed() {
            this.dirty = true;
            this.message = '';
            this.loadImages(); // выбрана картинка, которой ещё нет в кэше предпросмотра
        },

        numberInput(obj, key, ev) {
            const v = parseFloat(ev.target.value);
            if (!Number.isNaN(v)) { obj[key] = v; this.changed(); }
        },

        sigNames() { return this.layout.signals.map(s => s.name); },
        hasSignal(name) { return this.layout.signals.some(s => s.name === name); },
        signalTitle(name) { const s = this.layout.signals.find(x => x.name === name); return s ? (s.title || s.name) : name; },

        // ---- сигналы ----
        addSignal() {
            if (this.layout.signals.length >= RG_LIMITS.signals) return;
            let n = this.layout.signals.length + 1;
            while (this.hasSignal('sig' + n)) n++;
            this.layout.signals.push(rgNormSignal({ name: 'sig' + n }));
            this.changed();
        },

        removeSignal(i) {
            const s = this.layout.signals[i];
            const used = this.layout.screens.some(sc => sc.signal === s.name ||
                sc.widgets.some(w => w.signal === s.name));
            if (used && !confirm(this.t('ed_confirm_sig_used').replace('{n}', s.name))) return;
            this.layout.signals.splice(i, 1);
            this.sigOpen = -1;
            this.changed();
        },

        // Переименование идёт по ссылкам: экраны и виджеты следуют за новым именем.
        renameSignal(i, ev) {
            const s = this.layout.signals[i];
            const nn = ev.target.value.trim();
            const ok = /^[A-Za-z0-9_]{1,15}$/.test(nn) && !this.layout.signals.some((x, j) => j !== i && x.name === nn);
            if (!ok) { ev.target.value = s.name; this.say(this.t('ed_err_sig_name'), 'danger'); return; }
            for (const sc of this.layout.screens) {
                if (sc.signal === s.name) sc.signal = nn;
                for (const w of sc.widgets) if (w.signal === s.name) w.signal = nn;
            }
            delete this.manual[s.name];
            s.name = nn;
            this.changed();
        },

        addSignalZone(s) {
            if (s.zones.length >= RG_LIMITS.zones) return;
            s.zones.push({ from: s.min + (s.max - s.min) * 0.8, to: s.max, color: '#ff3030' });
            this.changed();
        },

        // ---- экраны ----
        newScreen(type) {
            if (!this.layout.signals.length) this.addSignal();
            const sig = this.layout.signals[0];
            const sc = rgNormScreen({ type, signal: sig.name });
            // Название, значение и единицы - обычные виджеты на привычных местах.
            const yTitle = type === 'ring' ? -70 : -90;
            const yValue = type === 'dial' ? 60 : 0;
            const yUnit = type === 'dial' ? 110 : 60;
            sc.widgets = [
                rgNormWidget({ type: 'text', text: sig.title || sig.name, y: yTitle }),
                rgNormWidget({ type: 'value', y: yValue }),
            ];
            if (sig.unit) sc.widgets.push(rgNormWidget({ type: 'text', text: sig.unit, y: yUnit }));
            return sc;
        },

        addScreen(type) {
            if (this.layout.screens.length >= RG_LIMITS.screens) return;
            this.layout.screens.push(this.newScreen(type));
            this.sel = this.layout.screens.length - 1;
            this.selW = -1;
            this.changed();
        },

        removeScreen() {
            if (this.layout.screens.length <= 1) return;
            this.layout.screens.splice(this.sel, 1);
            this.sel = Math.min(this.sel, this.layout.screens.length - 1);
            this.selW = -1;
            this.changed();
        },

        moveScreen(dir) {
            const j = this.sel + dir, a = this.layout.screens;
            if (j < 0 || j >= a.length) return;
            [a[this.sel], a[j]] = [a[j], a[this.sel]];
            this.sel = j;
            this.changed();
        },

        selectScreen(i) { this.sel = i; this.selW = -1; },

        addZone() {
            const sc = this.screen;
            if (sc.zones.length >= RG_LIMITS.zones) return;
            const sig = RgGauge.mainSig(this.layout, sc);
            sc.zones.push({ from: sig.min + (sig.max - sig.min) * 0.8, to: sig.max, color: '#ff3030' });
            this.changed();
        },

        addMarker() {
            const sc = this.screen;
            if (sc.markers.length >= RG_LIMITS.markers) return;
            const sig = RgGauge.mainSig(this.layout, sc);
            sc.markers.push({ value: sig.min + (sig.max - sig.min) * 0.9, color: '#ff3030' });
            this.changed();
        },

        // Своё значение диапазона на экране включается из текущего диапазона сигнала.
        toggleRange() {
            const sc = this.screen;
            if (sc.has_range) {
                const d = RgGauge.sigDef(this.layout, sc.signal);
                sc.min = d.min; sc.max = d.max;
            }
            this.changed();
        },

        typeLabel(type) { return this.t('ed_type_' + type); },
        screenName(sc, i) { return (i + 1) + '. ' + this.signalTitle(sc.signal) + ' · ' + this.typeLabel(sc.type); },

        // ---- виджеты ----
        widgetLabel(w) {
            const base = this.t('ed_w_' + w.type);
            if (w.type === 'text') return base + ': ' + w.text;
            if (w.type === 'image' || w.type === 'indicator') return base + (w.image ? ': ' + w.image : '');
            return base + ' · ' + this.signalTitle(w.signal || this.screen.signal);
        },

        addWidget(type) {
            const sc = this.screen;
            if (sc.widgets.length >= RG_LIMITS.widgets) return;
            const w = rgNormWidget({ type });
            if (type === 'text') w.text = 'Text';
            sc.widgets.push(w);
            this.selW = sc.widgets.length - 1;
            this.changed();
        },

        removeWidget(i) {
            this.screen.widgets.splice(i, 1);
            this.selW = -1;
            this.changed();
        },

        moveWidget(i, dir) {
            const a = this.screen.widgets, j = i + dir;
            if (j < 0 || j >= a.length) return;
            [a[i], a[j]] = [a[j], a[i]];
            this.selW = j;
            this.changed();
        },

        fontOptions() {
            return [{ v: '14', l: '14' }, { v: '28', l: '28' }, { v: '48', l: '48' }]
                .concat(this.media.files.filter(f => f.name.endsWith('.fnt')).map(f => ({ v: f.name, l: f.name })));
        },
        imageOptions() { return this.media.files.filter(f => f.name.endsWith('.bin')); },

        // ---- отправка на плату ----
        async apply() {
            if (this.tooBig || this.busy) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/layout', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: rgSerialize(this.layout),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.dirty = false;
                this.say(this.t('ed_applied'), 'success');
            } catch (e) {
                this.say(this.t('ed_err_apply') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        async reloadFromDevice() {
            if (this.dirty && !confirm(this.t('ed_confirm_discard'))) return;
            await this.loadAll();
            this.say(this.t('ed_reloaded'), 'success');
        },

        async resetDefault() {
            if (!confirm(this.t('ed_confirm_reset'))) return;
            const res = await this.authFetch('/api/layout/reset', { method: 'POST' });
            if (res.status === 401) return;
            if (!res.ok) { this.say(this.t('ed_err_apply'), 'danger'); return; }
            await this.loadAll();
            this.say(this.t('ed_reset_done'), 'success');
        },

        // ---- JSON ----
        toggleJson() {
            this.showJson = !this.showJson;
            if (this.showJson) this.jsonText = JSON.stringify(JSON.parse(rgSerialize(this.layout)), null, 2);
        },
        applyJsonText() {
            try {
                const layout = rgParseLayout(JSON.parse(this.jsonText));
                if (!layout.screens.length) throw new Error('screens');
                this.layout = layout;
                this.sel = 0;
                this.selW = -1;
                this.changed();
                this.loadImages();
            } catch (e) {
                this.say(this.t('ed_err_json'), 'danger');
            }
        },

        say(text, type) {
            this.message = text;
            this.messageType = type;
        },
    };
}
