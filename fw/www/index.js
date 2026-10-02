// Главная страница настройки. Пока только шапка: тема, язык, пароль.
// init() Alpine вызывает сам — x-init="init()" в разметке вызвал бы его дважды.
function mainPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        init() {
            this.checkAuthStatus();
        }
    };
}
