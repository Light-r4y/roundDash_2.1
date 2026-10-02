// Клиентская часть необязательной защиты паролем (см. common/config/auth.c). Вместо того
// чтобы полагаться на нативный диалог браузера при первом 401 на любой изменяющий
// запрос, пароль проверяется явно через /api/auth/verify (без побочных эффектов),
// заголовок Authorization кэшируется в sessionStorage (переживает перезагрузку
// страницы, но не переживает закрытие вкладки) и добавляется ко всем последующим
// изменяющим запросам через authFetch(). Кнопка-замочек в шапке страницы отражает
// текущее состояние и позволяет разблокировать/заблокировать доступ вручную.
const RG_AUTH_STORAGE_KEY = 'rg_auth_header';

function rgAuthMixin() {
    return {
        authRequired: false,
        authUnlocked: true,
        authHeaderValue: sessionStorage.getItem(RG_AUTH_STORAGE_KEY),

        showUnlockModal: false,
        unlockPassword: '',
        unlockBusy: false,
        unlockError: '',

        // Вызывать из init() каждой страницы - узнаёт, включена ли защита, и пробует
        // унаследовать ранее введённый в этой вкладке пароль (если он есть).
        async checkAuthStatus() {
            try {
                const res = await fetch('/api/auth/status');
                if (!res.ok) throw new Error('bad status ' + res.status);
                const data = await res.json();
                this.authRequired = !!data.enabled;
            } catch (error) {
                console.error('Auth status error:', error);
                this.authRequired = false;
            }

            if (!this.authRequired) {
                this.authUnlocked = true;
                return;
            }
            // Защита включена: разблокировано только если есть кэшированный заголовок
            // (оптимистично - настоящая проверка произойдёт при первом authFetch).
            this.authUnlocked = !!this.authHeaderValue;
        },

        // Обёртка над fetch() для изменяющих запросов: добавляет кэшированный пароль
        // (если есть) и автоматически "запирает" интерфейс при получении 401 (пароль
        // не задан/устарел/сменился на другом клиенте).
        async authFetch(url, options = {}) {
            const headers = Object.assign({}, options.headers || {});
            if (this.authHeaderValue) {
                headers['Authorization'] = this.authHeaderValue;
            }
            const res = await fetch(url, Object.assign({}, options, { headers }));
            if (res.status === 401) {
                this.lockAuth();
                this.showUnlockModal = true;
            }
            return res;
        },

        // Клик по кнопке-замочку: разблокировано -> запереть обратно; заперто -> окно ввода пароля.
        toggleLockButton() {
            if (this.authUnlocked) {
                this.lockAuth();
            } else {
                this.unlockError = '';
                this.unlockPassword = '';
                this.showUnlockModal = true;
            }
        },

        lockAuth() {
            this.authUnlocked = false;
            this.authHeaderValue = null;
            sessionStorage.removeItem(RG_AUTH_STORAGE_KEY);
        },

        async submitUnlock() {
            if (!this.unlockPassword) return;
            this.unlockBusy = true;
            this.unlockError = '';
            const header = 'Basic ' + btoa('rg:' + this.unlockPassword);
            try {
                const res = await fetch('/api/auth/verify', {
                    method: 'POST',
                    headers: { 'Authorization': header }
                });
                if (!res.ok) throw new Error('bad status ' + res.status);
                this.authHeaderValue = header;
                this.authUnlocked = true;
                sessionStorage.setItem(RG_AUTH_STORAGE_KEY, header);
                this.showUnlockModal = false;
                this.unlockPassword = '';
            } catch (error) {
                this.unlockError = this.t ? this.t('auth_unlock_error') : 'Wrong password';
            } finally {
                this.unlockBusy = false;
            }
        },

        // Управляющие элементы (переключатели/кнопки сохранения) должны быть недоступны,
        // пока пароль задан и не введён в этой вкладке. Метод, а не getter - объекты,
        // возвращаемые *Mixin()-функциями, всегда копируются через object spread
        // ({...rgAuthMixin()}), а spread считывает getter'ы один раз в момент копирования
        // (замораживая значение), тогда как обычный метод вызывается заново при каждом
        // обращении и видит актуальные authRequired/authUnlocked.
        controlsLocked() {
            return this.authRequired && !this.authUnlocked;
        }
    };
}
