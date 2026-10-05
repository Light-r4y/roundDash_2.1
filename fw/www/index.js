// Главная страница: состояние устройства, переходы в разделы, яркость и пароль на настройки.
// init() Alpine вызывает сам — x-init="init()" в разметке вызвал бы его дважды.
const RG_RESET_REASONS = ['unknown', 'poweron', 'ext', 'sw', 'panic', 'int_wdt', 'task_wdt', 'wdt', 'deepsleep',
    'brownout', 'sdio', 'usb', 'jtag', 'efuse', 'pwr_glitch', 'cpu_lockup'];
const RG_PASSWORD_MIN = 4, RG_PASSWORD_MAX = 64;

function mainPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        status: null,   // /api/status
        can: null,      // /api/can
        canRate: 0,
        lastRx: null,
        media: null,    // /api/media
        layout: null,   // сводка по /api/layout: { screens, signals }
        brightness: null,
        brightTimer: null,

        pwNew: '',
        pwRepeat: '',
        busy: false,
        message: '',
        messageType: 'success',

        init() {
            this.checkAuthStatus();
            this.pollAll(true);
            setInterval(() => this.pollAll(false), 3000);
        },

        async getJson(url) {
            try {
                const res = await fetch(url, { cache: 'no-store' });
                return res.ok ? await res.json() : null;
            } catch (e) {
                return null;
            }
        },

        // once - то, что не меняется само (раскладка, файлы, яркость), читаем один раз.
        async pollAll(once) {
            const [status, can] = await Promise.all([this.getJson('/api/status'), this.getJson('/api/can')]);
            this.status = status;
            if (can) {
                const now = performance.now();
                if (this.lastRx) {
                    this.canRate = Math.max(0, Math.round((can.rx - this.lastRx.rx) * 1000 / (now - this.lastRx.t)));
                }
                this.lastRx = { rx: can.rx, t: now };
            }
            this.can = can;
            if (!once) return;
            const [media, layout, display] = await Promise.all([
                this.getJson('/api/media'), this.getJson('/api/layout'), this.getJson('/api/display')]);
            this.media = media;
            if (layout) {
                this.layout = { screens: (layout.screens || []).length,
                    signals: Array.isArray(layout.signals) ? layout.signals.length : null };
            }
            if (display) this.brightness = display.brightness;
        },

        // ---- подписи плиток ----
        subScreens() {
            if (!this.layout) return '';
            return this.t('tile_screens').replace('{n}', this.layout.screens);
        },
        subMedia() {
            return this.media ? this.t('tile_media').replace('{n}', this.media.files.length) : '';
        },
        subCan() {
            if (!this.can) return '';
            return this.can.demo ? this.t('tile_can_demo') : this.canLabel();
        },

        // ---- состояние ----
        fmtUptime(s) {
            const d = Math.floor(s / 86400), h = Math.floor(s % 86400 / 3600), m = Math.floor(s % 3600 / 60);
            const parts = [];
            if (d) parts.push(d + ' ' + this.t('u_d'));
            if (d || h) parts.push(h + ' ' + this.t('u_h'));
            parts.push(m + ' ' + this.t('u_m'));
            if (!d && !h) parts.push(Math.floor(s % 60) + ' ' + this.t('u_s'));
            return parts.join(' ');
        },
        // Плата отдаёт номер esp_reset_reason_t; мок и старые версии - строку.
        resetReason(r) {
            const name = typeof r === 'number' ? (RG_RESET_REASONS[r] || 'unknown') : String(r || 'unknown');
            const key = 'rr_' + name;
            const label = this.t(key);
            return label === key ? name : label;
        },
        fmtSize(b) {
            return b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : b >= 1024 ? Math.round(b / 1024) + ' KB' : b + ' B';
        },
        canLabel() {
            const c = this.can;
            if (!c) return '...';
            if (!c.node_up) return this.t('can_state_down');
            return this.t('can_state_' + (['active', 'warning', 'passive', 'bus_off'][c.state] || 'active'));
        },
        canClass() {
            const c = this.can;
            if (!c || !c.node_up || c.state >= 3) return 'bg-danger';
            return c.state === 0 ? 'bg-success' : 'bg-warning text-dark';
        },

        // ---- яркость: отправляем, когда ползунок остановился ----
        onBrightness(ev) {
            this.brightness = Number(ev.target.value);
            clearTimeout(this.brightTimer);
            this.brightTimer = setTimeout(() => this.sendBrightness(), 250);
        },
        async sendBrightness() {
            try {
                const res = await this.authFetch('/api/display', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ brightness: this.brightness }),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            }
        },

        // ---- пароль на настройки ----
        async savePassword() {
            if (this.busy || !this.pwNew) return;
            if (this.pwNew.length < RG_PASSWORD_MIN || this.pwNew.length > RG_PASSWORD_MAX) {
                this.say(this.t('pw_err_len').replace('{a}', RG_PASSWORD_MIN).replace('{b}', RG_PASSWORD_MAX), 'danger');
                return;
            }
            if (this.pwNew !== this.pwRepeat) {
                this.say(this.t('pw_err_match'), 'danger');
                return;
            }
            await this.sendPassword(this.pwNew);
        },
        async removePassword() {
            if (this.busy || !confirm(this.t('pw_confirm_remove'))) return;
            await this.sendPassword('');
        },
        async sendPassword(pw) {
            this.busy = true;
            try {
                const res = await this.authFetch('/api/auth/password', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ password: pw }),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                // Вкладка остаётся разблокированной: заголовок пересчитываем под новый пароль.
                if (pw) {
                    const header = 'Basic ' + btoa('rg:' + pw);
                    this.authHeaderValue = header;
                    sessionStorage.setItem(RG_AUTH_STORAGE_KEY, header);
                    this.authRequired = true;
                    this.authUnlocked = true;
                } else {
                    this.authHeaderValue = null;
                    sessionStorage.removeItem(RG_AUTH_STORAGE_KEY);
                    this.authRequired = false;
                    this.authUnlocked = true;
                }
                this.pwNew = '';
                this.pwRepeat = '';
                this.say(this.t(pw ? 'pw_saved' : 'pw_removed'), 'success');
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        say(text, type) {
            this.message = text;
            this.messageType = type;
        },
    };
}
