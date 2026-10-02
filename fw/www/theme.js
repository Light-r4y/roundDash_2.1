// Тема интерфейса (светлая/тёмная). Выбранная тема хранится в localStorage
// ('rg_theme') - применяется атрибутом data-theme на <html>, который читает
// style.css. Устанавливается сразу при загрузке скрипта (не дожидаясь Alpine),
// чтобы страница не мигала неправильной темой при отрисовке.
const RG_THEME_STORAGE_KEY = 'rg_theme';

function rgApplyTheme(theme) {
    document.documentElement.setAttribute('data-theme', theme);
}

(function rgInitTheme() {
    const saved = localStorage.getItem(RG_THEME_STORAGE_KEY);
    const theme = saved === 'light' || saved === 'dark'
        ? saved
        : (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
    rgApplyTheme(theme);
})();

function rgThemeMixin() {
    return {
        theme: document.documentElement.getAttribute('data-theme') || 'dark',

        toggleTheme() {
            this.theme = this.theme === 'dark' ? 'light' : 'dark';
            localStorage.setItem(RG_THEME_STORAGE_KEY, this.theme);
            rgApplyTheme(this.theme);
        }
    };
}
