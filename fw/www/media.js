// Страница "Медиа": картинки и шрифты на плате (раздел media). Показывает файлы с
// миниатюрами и отметкой "используется", загружает новые (PNG/JPG превращаются в
// LVGL .bin прямо в браузере, шрифт - готовый LVGL bin font) и удаляет лишние.
const RG_THUMB = 88; // сторона миниатюры, px

// Имена файлов, на которые ссылается layout (картинки фонов, стрелок, виджетов; шрифты виджетов).
function rgUsedFiles(layout) {
    const names = new Set();
    const keys = ['bg_image', 'needle_image', 'image', 'font'];
    const walk = (o) => {
        if (Array.isArray(o)) { o.forEach(walk); return; }
        if (o && typeof o === 'object') {
            for (const [k, v] of Object.entries(o)) {
                if (keys.includes(k) && typeof v === 'string' && v) names.add(v);
                else walk(v);
            }
        }
    };
    walk(layout.screens || []);
    return names;
}

function mediaPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        media: { files: [], total: 0, used: 0, max_file: 1000000 },
        usedNames: [],
        thumbs: {},          // имя -> data URL миниатюры
        thumbSize: {},       // имя -> размер файла, для которого миниатюра сделана
        loaded: false,

        uploadName: '',
        uploadFile: null,
        uploadInfo: '',
        uploadThumb: '',
        uploadKind: 'image',

        busy: false,
        message: '',
        messageType: 'success',

        init() {
            this.checkAuthStatus();
            this.loadAll();
        },

        async loadAll() {
            await Promise.all([this.loadMedia(), this.loadUsed()]);
            this.loaded = true;
            this.loadThumbs();
        },

        async loadMedia() {
            try {
                const res = await fetch('/api/media', { cache: 'no-store' });
                if (!res.ok) throw new Error('HTTP ' + res.status);
                this.media = await res.json();
            } catch (e) {
                this.say(this.t('can_err_load') + ': ' + e.message, 'danger');
            }
        },

        async loadUsed() {
            try {
                const res = await fetch('/api/layout', { cache: 'no-store' });
                if (res.ok) this.usedNames = [...rgUsedFiles(await res.json())];
            } catch (e) { /* без списка просто нет отметок "используется" */ }
        },

        // Миниатюры качаем по одной: плата отдаёт файлы по Wi-Fi AP небыстро.
        async loadThumbs() {
            for (const f of this.media.files) {
                if (!this.isImage(f.name) || this.thumbSize[f.name] === f.size) continue;
                try {
                    const res = await fetch('/media/' + encodeURIComponent(f.name), { cache: 'no-store' });
                    const canvas = res.ok ? RgLvBin.decode(await res.arrayBuffer()) : null;
                    if (canvas) {
                        this.thumbs = { ...this.thumbs, [f.name]: this.toThumb(canvas) };
                        this.thumbSize[f.name] = f.size;
                    }
                } catch (e) { /* без миниатюры остаётся значок */ }
            }
        },

        toThumb(canvas) {
            const k = Math.min(1, RG_THUMB / Math.max(canvas.width, canvas.height));
            const c = document.createElement('canvas');
            c.width = Math.max(1, Math.round(canvas.width * k));
            c.height = Math.max(1, Math.round(canvas.height * k));
            c.getContext('2d').drawImage(canvas, 0, 0, c.width, c.height);
            return c.toDataURL('image/png');
        },

        isImage(name) { return name.endsWith('.bin'); },
        isFont(name) { return name.endsWith('.fnt'); },
        isUsed(name) { return this.usedNames.includes(name); },
        get images() { return this.media.files.filter(f => this.isImage(f.name)); },
        get fonts() { return this.media.files.filter(f => this.isFont(f.name)); },
        get usedPct() { return this.media.total ? Math.min(100, this.media.used * 100 / this.media.total) : 0; },

        // Что за файл по содержимому: картинка LVGL (магия 0x19), шрифт LVGL (метка 'head' в
        // заголовке) или ничего.
        sniff(buf) {
            const u8 = new Uint8Array(buf);
            if (RgLvBin.decode(buf)) return 'image';
            if (u8.length > 8 && String.fromCharCode(u8[4], u8[5], u8[6], u8[7]) === 'head') return 'font';
            return null;
        },

        async pickFile(ev) {
            const file = ev.target.files[0];
            this.uploadFile = null;
            this.uploadInfo = '';
            this.uploadThumb = '';
            if (!file) return;
            try {
                let conv, kind = 'image';
                if (/\.(bin|fnt)$/i.test(file.name)) {
                    const buf = await file.arrayBuffer();
                    kind = this.sniff(buf);
                    if (!kind) throw new Error(this.t('ed_err_bin'));
                    conv = { buf, w: 0, h: 0, alpha: false };
                } else {
                    conv = await RgLvBin.fromImageFile(file);
                }
                this.uploadKind = kind;
                this.uploadFile = conv;
                const base = file.name.replace(/\.[^.]*$/, '').replace(/[^A-Za-z0-9_-]/g, '_').slice(0, 27) || 'file';
                this.uploadName = base + (kind === 'font' ? '.fnt' : '.bin');
                this.uploadInfo = (kind === 'font' ? this.t('ed_font') + ', ' : '')
                    + (conv.w ? conv.w + 'x' + conv.h + ', ' + (conv.alpha ? 'ARGB8888' : 'RGB565') + ', ' : '')
                    + this.fmtSize(conv.buf.byteLength);
                if (kind === 'image') {
                    const c = RgLvBin.decode(conv.buf);
                    if (c) this.uploadThumb = this.toThumb(c);
                }
                if (conv.buf.byteLength > this.media.max_file) {
                    this.uploadInfo += ' - ' + this.t('ed_too_large');
                    this.uploadFile = null;
                }
            } catch (e) {
                this.say(this.t('ed_err_image') + ': ' + e.message, 'danger');
            }
        },

        async upload() {
            if (!this.uploadFile || !/^(?!\.)[A-Za-z0-9._-]{1,31}$/.test(this.uploadName) || this.busy) return;
            if (this.media.files.some(f => f.name === this.uploadName) &&
                !confirm(this.t('ed_confirm_replace').replace('{n}', this.uploadName))) return;
            this.busy = true;
            try {
                const res = await this.authFetch('/api/media?name=' + encodeURIComponent(this.uploadName), {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/octet-stream' },
                    body: this.uploadFile.buf,
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                delete this.thumbSize[this.uploadName];
                this.uploadFile = null;
                this.uploadInfo = '';
                this.uploadThumb = '';
                this.$refs.fileInput.value = '';
                await this.loadMedia();
                this.say(this.t('ed_uploaded'), 'success');
                this.loadThumbs();
            } catch (e) {
                this.say(this.t('ed_err_upload') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },

        async removeMedia(name) {
            if (!confirm(this.t('ed_confirm_delete').replace('{n}', name))) return;
            const res = await this.authFetch('/api/media/delete?name=' + encodeURIComponent(name), { method: 'POST' });
            if (res.status === 401) return;
            if (!res.ok) { this.say(this.t('ed_err_upload'), 'danger'); return; }
            await this.loadMedia();
        },

        fmtSize(b) {
            return b >= 1048576 ? (b / 1048576).toFixed(1) + ' MB' : b >= 1024 ? Math.round(b / 1024) + ' KB' : b + ' B';
        },

        say(text, type) {
            this.message = text;
            this.messageType = type;
        },
    };
}
