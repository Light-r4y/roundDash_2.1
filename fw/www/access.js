// Страница "Доступ": пароль на настройки и параметры Wi-Fi точки доступа.
// Менять Wi-Fi можно только когда пароль на настройки задан (проверяет и сама плата).
const RG_PASSWORD_MIN = 4, RG_PASSWORD_MAX = 64;
const RG_WIFI_PASS_MIN = 8, RG_WIFI_PASS_MAX = 64, RG_WIFI_SSID_MAX = 32;

function accessPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        pwNew: '',
        pwRepeat: '',
        wifi: null,        // /api/wifi
        wifiFailed: false,
        wifiSsid: '',
        wifiPass: '',
        wifiShow: false,
        busy: false,
        message: '',
        messageType: 'success',

        async init() {
            this.checkAuthStatus();
            await this.loadWifi();
        },

        controlsLocked() {
            return this.authRequired && !this.authUnlocked;
        },
        // Wi-Fi закрыт и тогда, когда пароль на настройки не задан вовсе.
        wifiLocked() {
            return !this.authRequired || this.controlsLocked();
        },

        async loadWifi() {
            try {
                const res = await fetch('/api/wifi', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                this.wifi = await res.json();
                this.wifiSsid = this.wifi.custom_ssid ? this.wifi.ssid : '';
                this.wifiFailed = false;
            } catch (e) {
                this.wifi = null;
                this.wifiFailed = true;
                this.say(this.t('home_no_link'), 'danger');
            }
        },

        // ---- Wi-Fi ----
        async saveWifi() {
            if (this.busy || !this.wifiPass) return;
            const ssid = this.wifiSsid.trim();
            if (new TextEncoder().encode(ssid).length > RG_WIFI_SSID_MAX) {
                this.say(this.t('wifi_err_ssid'), 'danger');
                return;
            }
            if (this.wifiPass.length < RG_WIFI_PASS_MIN || this.wifiPass.length > RG_WIFI_PASS_MAX) {
                this.say(this.t('wifi_err_pass').replace('{a}', RG_WIFI_PASS_MIN).replace('{b}', RG_WIFI_PASS_MAX), 'danger');
                return;
            }
            if (!confirm(this.t('wifi_confirm'))) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/wifi', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ ssid, password: this.wifiPass }),
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                this.wifiPass = '';
                await this.loadWifi();
                this.say(this.t('wifi_saved'), 'success');
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
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
