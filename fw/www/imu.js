// Страница "Датчик": живые показания акселерометра, калибровка по силе тяжести и выбор
// направления "вперёд" (tasks/imu). Точка рисуется тем же отрисовщиком, что и экран
// перегрузок на плате.
const RG_IMU_POLL_MS = 120;

// Экран-образец для отрисовки: как встроенный д-метр, без дополнительных виджетов.
const RG_IMU_SCREEN = {
    type: 'gmeter', signal: 'g_lon', signal2: 'g_lat', g_range: 1.5, g_step: 0.5, trail: 8, peaks: true,
    felt: true, color: '#00c0ff', text_color: '#505050', bg_color: '#000000', bg_image: '',
    zones: [], markers: [], widgets: [],
};

function imuPage() {
    return {
        ...rgI18nMixin(),
        ...rgAuthMixin(),
        ...rgThemeMixin(),

        s: null,        // последний ответ /api/imu
        offline: false, // плата не отвечает
        gm: {},         // шлейф и максимумы для отрисовки
        stopped: false,
        busy: false,
        message: '',
        messageType: 'success',

        init() {
            this.checkAuthStatus();
            this.pollLoop();
            const draw = (t) => {
                this.draw(t);
                requestAnimationFrame(draw);
            };
            requestAnimationFrame(draw);
        },

        // Опрос по цепочке, а не по таймеру: следующий запрос только после ответа на прошлый.
        async pollLoop() {
            while (!this.stopped) {
                try {
                    const res = await fetch('/api/imu', { cache: 'no-store' });
                    if (res.ok) { this.s = await res.json(); this.offline = false; } else this.offline = true;
                } catch (e) {
                    this.offline = true;
                }
                await new Promise(r => setTimeout(r, RG_IMU_POLL_MS));
            }
        },

        draw(t) {
            const canvas = this.$refs.canvas;
            if (!canvas) return;
            const s = this.s;
            const values = s && s.ok ? { g_lon: s.lon, g_lat: s.lat } : {};
            RgGauge.gmeterTrack(this.gm, RG_IMU_SCREEN, values, t);
            RgGauge.draw(canvas, { signals: [], screens: [] }, RG_IMU_SCREEN, values, {}, -1, false, this.gm);
        },

        get sensorOk() { return !!(this.s && this.s.ok); },
        fmt(v) { return v == null ? '' : (v >= 0 ? '+' : '') + Number(v).toFixed(2); },

        // ---- состояние калибровки и направления ----
        calText() {
            const s = this.s;
            if (!s) return '';
            if (s.cal === 'running') return this.t('imu_cal_running');
            if (s.cal === 'moved') return this.t('imu_cal_moved');
            if (s.cal === 'done') return this.t('imu_cal_done');
            return s.calibrated ? this.t('imu_cal_have') : this.t('imu_cal_none');
        },
        calClass() {
            const s = this.s;
            if (!s) return 'text-muted';
            if (s.cal === 'moved') return 'text-fault';
            if (s.cal === 'running') return 'text-warning';
            return s.calibrated ? 'text-success' : 'text-muted';
        },
        detectText() {
            const d = this.s && this.s.detect;
            return d === 'armed' ? this.t('imu_fwd_armed') : d === 'done' ? this.t('imu_fwd_done')
                : d === 'timeout' ? this.t('imu_fwd_timeout') : '';
        },

        // ---- действия ----
        async post(url, body, okKey) {
            if (this.busy) return;
            this.busy = true;
            try {
                const res = await this.authFetch(url, {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: body ? JSON.stringify(body) : undefined,
                });
                if (res.status === 401) return;
                if (!res.ok) throw new Error((await res.text()) || ('HTTP ' + res.status));
                if (okKey) this.say(this.t(okKey), 'success');
            } catch (e) {
                this.say(this.t('set_err') + ': ' + e.message, 'danger');
            } finally {
                this.busy = false;
            }
        },
        calibrate() { return this.post('/api/imu/calibrate', null, 'imu_cal_started'); },
        detectForward() { return this.post('/api/imu/forward', { detect: true }, 'imu_fwd_started'); },
        setForward(n) { return this.post('/api/imu/forward', { fwd: n }, null); },

        say(text, type) {
            this.message = text;
            this.messageType = type;
        },
    };
}
