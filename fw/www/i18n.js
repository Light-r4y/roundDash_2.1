// Словарь интерфейса RU/EN. Выбранный язык хранится в localStorage ('rg_lang').
// Механизм перенесён из wifi-serv, словарь начат заново: здесь только ключи
// шапки, окна пароля и страницы-заглушки.
const RG_I18N = {
    ru: {
        title_main: 'roundGauge - Настройка',
        nav_update: 'Обновление',
        theme_toggle: 'Сменить тему оформления',
        placeholder: 'Настройка приборки появится здесь.',
        checking: 'Проверка...',
        cancel: 'Отмена',
        auth_lock_tooltip: 'Заблокировать управление',
        auth_unlock_tooltip: 'Ввести пароль для разблокировки',
        auth_locked_hint: 'Задан пароль - разблокируйте "кнопкой-замочком" в шапке',
        auth_unlock_title: 'Ввод пароля',
        auth_unlock_password: 'Пароль',
        auth_unlock_submit: 'Разблокировать',
        auth_unlock_error: 'Неверный пароль'
    },
    en: {
        title_main: 'roundGauge - Settings',
        nav_update: 'Update',
        theme_toggle: 'Switch color theme',
        placeholder: 'Dashboard settings will appear here.',
        checking: 'Checking...',
        cancel: 'Cancel',
        auth_lock_tooltip: 'Lock controls',
        auth_unlock_tooltip: 'Enter password to unlock',
        auth_locked_hint: 'Password set - unlock with the "lock" button in the header',
        auth_unlock_title: 'Enter password',
        auth_unlock_password: 'Password',
        auth_unlock_submit: 'Unlock',
        auth_unlock_error: 'Wrong password'
    }
};

// Подмешивается в Alpine-компоненты страниц: this.t('key') и this.toggleLang().
function rgI18nMixin() {
    const lang = localStorage.getItem('rg_lang') || 'ru';
    document.documentElement.lang = lang;

    return {
        lang,

        t(key) {
            const dict = RG_I18N[this.lang] || RG_I18N.ru;
            return dict[key] || RG_I18N.ru[key] || key;
        },

        toggleLang() {
            this.lang = this.lang === 'ru' ? 'en' : 'ru';
            localStorage.setItem('rg_lang', this.lang);
            document.documentElement.lang = this.lang;
        }
    };
}
