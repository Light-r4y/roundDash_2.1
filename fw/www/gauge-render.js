// Предпросмотр экрана приборки на canvas 480x480. Повторяет геометрию прошивки
// (fw/tasks/ui/ui_screens.c): шкала 440 px, стрелка короче радиуса на 55 px,
// подписи делений, цветовые зоны, кольцо с заливкой по зоне, дополнительные
// виджеты по координатам от центра. Это приближение: настоящий результат - на
// самой плате; свои шрифты (.fnt) здесь рисуются запасным шрифтом.
const RgGauge = (() => {
    const SIZE = 480, C = SIZE / 2, R = 220;
    const NEEDLE_MARGIN = 55;
    const FONT = "'Montserrat', 'Segoe UI', Arial, sans-serif";
    const GREY = '#9e9e9e';
    const RAD = Math.PI / 180;
    const BLINK_HALF_MS = 250;

    const frac = (min, max, v) => Math.min(1, Math.max(0, (v - min) / (max - min)));
    const zoneAt = (zones, v) => (zones || []).find(z => v >= z.from && v <= z.to) || null;

    // Параметры сигнала как в прошивке (resolve_signal): таблица layout; для основного
    // виджета экрана диапазон и зоны экрана - поверх.
    function sigDef(layout, name) {
        return layout.signals.find(s => s.name === name) || { name, min: 0, max: 100, decimals: 0, zones: [] };
    }
    function mainSig(layout, sc) {
        const d = sigDef(layout, sc.signal);
        return {
            min: sc.has_range ? sc.min : d.min,
            max: sc.has_range ? sc.max : d.max,
            decimals: d.decimals,
            zones: sc.zones.length ? sc.zones : d.zones,
        };
    }
    function widgetSig(layout, sc, w) {
        const d = sigDef(layout, w.signal || sc.signal);
        return { name: d.name, min: d.min, max: d.max, zones: d.zones,
                 decimals: w.decimals >= 0 ? w.decimals : d.decimals };
    }

    // Размер шрифта в предпросмотре: встроенные 14/28/48; у своего .fnt - число перед расширением
    // в имени (seg7_120.fnt -> 120), иначе 28. Шрифт файла браузер не знает, рисуется запасным.
    // Имя файла шрифта -> шаблон CSS-шрифта с {px} (для обзорных картинок: '800 {px} Orbitron'). В редакторе пусто:
    // свой .fnt рисуется запасным шрифтом.
    const fontMap = {};
    const fontCss = (name, px) => (fontMap[name] ? fontMap[name].replace('{px}', px + 'px') : px + 'px ' + FONT);

    const fontPx = (name) => {
        if (name === '14') return 14;
        if (name === '48') return 48;
        const m = /_(\d{1,3})\.fnt$/i.exec(name || '');
        return m && +m[1] >= 8 && +m[1] <= 200 ? +m[1] : 28;
    };

    function labelText(sc, sig, f) {
        const x = (sig.min + (sig.max - sig.min) * f) / (sc.label_div || 1);
        return Math.abs(x - Math.round(x)) < 0.05 ? String(Math.round(x)) : x.toFixed(1);
    }

    function polar(r, ang) {
        return [C + r * Math.cos(ang), C + r * Math.sin(ang)];
    }

    function line(ctx, p, q, width, color, cap) {
        ctx.beginPath();
        ctx.moveTo(p[0], p[1]);
        ctx.lineTo(q[0], q[1]);
        ctx.lineWidth = width;
        ctx.strokeStyle = color;
        ctx.lineCap = cap || 'butt';
        ctx.stroke();
    }

    // ---- основной виджет ----
    function drawDial(ctx, sc, sig, v, imgs) {
        const a0 = sc.rotation * RAD, span = sc.angle * RAD;

        for (const z of sig.zones) {
            ctx.beginPath();
            ctx.arc(C, C, R - 4, a0 + span * frac(sig.min, sig.max, z.from), a0 + span * frac(sig.min, sig.max, z.to));
            ctx.lineWidth = 8;
            ctx.strokeStyle = z.color;
            ctx.lineCap = 'butt';
            ctx.stroke();
        }

        const n = sc.ticks;
        for (let i = 0; i < n; i++) {
            const f = i / (n - 1), ang = a0 + span * f, major = i % sc.major_every === 0;
            const zone = zoneAt(sig.zones, sig.min + (sig.max - sig.min) * f);
            const len = major ? sc.tick_major_len : sc.tick_minor_len;
            line(ctx, polar(R - len, ang), polar(R, ang), major ? sc.tick_major_width : sc.tick_minor_width,
                zone ? zone.color : (major ? sc.color : GREY));
        }
        const nlab = Math.floor((n - 1) / sc.major_every) + 1;
        for (let k = 0; k < nlab; k++) {
            const f = nlab > 1 ? (k * sc.major_every) / (n - 1) : 0;
            const zone = zoneAt(sig.zones, sig.min + (sig.max - sig.min) * f);
            // Как на плате (lv_scale): радиус подписи = край - длина крупной риски - зазор.
            const [x, y] = polar(R - sc.tick_major_len - sc.label_gap, a0 + span * f);
            ctx.font = fontCss(sc.label_font, fontPx(sc.label_font));
            ctx.fillStyle = zone ? zone.color : sc.text_color;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(labelText(sc, sig, f), x, y);
        }

        // Метки: короткая черта поперёк делений.
        for (const m of sc.markers) {
            const ang = a0 + span * frac(sig.min, sig.max, m.value);
            line(ctx, polar(R - 40, ang), polar(R - 2, ang), 4, m.color);
        }

        // Стрелка. Нет данных - в начале шкалы, как на плате.
        const ang = a0 + span * (v == null ? 0 : frac(sig.min, sig.max, v));
        const img = sc.needle_image ? imgs[sc.needle_image] : null;
        if (img) {
            ctx.save();
            ctx.translate(C, C);
            ctx.rotate(ang);
            // Ось вращения: точка картинки (needle_px, needle_py), -1 - авто (слева по центру).
            ctx.drawImage(img, -(sc.needle_px >= 0 ? sc.needle_px : 0), -(sc.needle_py >= 0 ? sc.needle_py : img.height / 2));
            ctx.restore();
        } else {
            line(ctx, [C, C], polar(R - NEEDLE_MARGIN, ang), sc.needle_width, sc.needle_color, 'round');
        }
    }

    function arcShape(ctx, cx, cy, diameter, width, angle, rotation, bg, fg, f) {
        const a0 = rotation * RAD, span = angle * RAD, r = Math.max(0, diameter / 2 - width / 2);
        ctx.lineWidth = width;
        ctx.lineCap = 'butt';
        ctx.beginPath();
        ctx.arc(cx, cy, r, a0, a0 + span);
        ctx.strokeStyle = bg;
        ctx.stroke();
        if (f > 0) {
            ctx.beginPath();
            ctx.arc(cx, cy, r, a0, a0 + span * f);
            ctx.strokeStyle = fg;
            ctx.stroke();
        }
    }

    function drawRing(ctx, sc, sig, v) {
        const zone = v == null ? null : zoneAt(sig.zones, v);
        arcShape(ctx, C, C, SIZE - 40, sc.ring_width, sc.angle, sc.rotation, '#303030',
            zone ? zone.color : sc.color, v == null ? 0 : frac(sig.min, sig.max, v));
    }

    // ---- G-сенсор: точка на круговой сетке (ui_screens.c, build_gmeter / update_gmeter) ----
    const GM_RADIUS = 180, GM_DOT_D = 28, GM_CLAMP = 1.05;

    // Положение точки по значениям двух сигналов: sx/sy в g (вправо и вверх на экране), px/py -
    // пиксели от центра. null - нет данных. Точка показывает силу, которую чувствует водитель
    // (felt): при торможении вверх, при правом повороте влево.
    function gmeterPoint(sc, values) {
        const lon = values[sc.signal], lat = values[sc.signal2];
        if (lon == null || lat == null) return null;
        const scale = GM_RADIUS / sc.g_range;
        let sx = sc.felt ? -lat : lat, sy = sc.felt ? -lon : lon;
        const m = Math.hypot(sx, sy), lim = sc.g_range * GM_CLAMP;
        if (m > lim) { sx *= lim / m; sy *= lim / m; }
        return { sx, sy, px: Math.round(sx * scale), py: -Math.round(sy * scale) };
    }

    // Шлейф и максимумы G-сенсора, как в ui_screens.c: точка записывается каждые 50 мс, максимум
    // держится 8 с и потом спадает на 0,25 g/с. st - { sc, hist, peaks, peakMs, trailMs, lastMs }
    // (пустой объект подходит), его ведёт вызывающий и отдаёт в draw() как extra.
    function gmeterTrack(st, sc, values, now) {
        if (st.sc !== sc) {
            Object.assign(st, { sc, hist: [], peaks: [0, 0, 0, 0], peakMs: [now, now, now, now], trailMs: now, lastMs: now });
        }
        const pt = gmeterPoint(sc, values);
        if (!pt) { st.hist = []; return; }
        if (now - st.trailMs >= 50 || !st.hist.length) {
            st.trailMs = now;
            st.hist.unshift({ px: pt.px, py: pt.py });
            st.hist.length = Math.min(st.hist.length, sc.trail + 1);
        }
        const dt = Math.min(0.2, (now - st.lastMs) / 1000);
        st.lastMs = now;
        const cur = [Math.max(pt.sy, 0), Math.max(-pt.sy, 0), Math.max(-pt.sx, 0), Math.max(pt.sx, 0)];
        for (let i = 0; i < 4; i++) {
            if (cur[i] >= st.peaks[i]) { st.peaks[i] = cur[i]; st.peakMs[i] = now; }
            else if (now - st.peakMs[i] > 8000) st.peaks[i] = Math.max(cur[i], st.peaks[i] - 0.25 * dt);
        }
    }

    // extra: { hist: [{px, py}, ...] - шлейф от новой к старой, peaks: [вверх, вниз, влево, вправо] в g }.
    function drawGmeter(ctx, sc, values, extra) {
        const scale = GM_RADIUS / sc.g_range;
        for (let k = 1; k * sc.g_step <= sc.g_range + 0.001; k++) {
            ctx.beginPath();
            ctx.arc(C, C, k * sc.g_step * scale, 0, Math.PI * 2);
            ctx.lineWidth = k * sc.g_step >= sc.g_range - 0.001 ? 3 : 2;
            ctx.strokeStyle = sc.text_color;
            ctx.stroke();
        }
        ctx.fillStyle = sc.text_color;
        ctx.fillRect(C - 1, C - GM_RADIUS, 2, GM_RADIUS * 2);
        ctx.fillRect(C - GM_RADIUS, C - 1, GM_RADIUS * 2, 2);

        const pt = gmeterPoint(sc, values);
        if (!pt) return;

        if (sc.peaks && extra && extra.peaks) {
            const ax = [0, 0, -1, 1], ay = [-1, 1, 0, 0];
            const lx = [0, 0, -(GM_RADIUS + 36), GM_RADIUS + 36], ly = [-(GM_RADIUS + 24), GM_RADIUS + 24, 0, 0];
            for (let i = 0; i < 4; i++) {
                const v = extra.peaks[i];
                if (!(v > 0.05)) continue;
                const d = Math.round(v * scale);
                ctx.beginPath();
                ctx.arc(C + ax[i] * d, C + ay[i] * d, 5, 0, Math.PI * 2);
                ctx.fillStyle = '#ff4040';
                ctx.fill();
                ctx.font = '14px ' + FONT;
                ctx.fillStyle = '#ff8080';
                ctx.textAlign = 'center';
                ctx.textBaseline = 'middle';
                ctx.fillText(v.toFixed(2), C + lx[i], C + ly[i]);
            }
        }

        // Шлейф: самые старые точки мельче и прозрачнее.
        const n = Math.min(sc.trail, extra && extra.hist ? extra.hist.length - 1 : 0);
        for (let k = n - 1; k >= 0; k--) {
            const h = extra.hist[k + 1];
            ctx.beginPath();
            ctx.arc(C + h.px, C + h.py, (8 + (GM_DOT_D - 12) * (sc.trail - k) / (sc.trail + 1)) / 2, 0, Math.PI * 2);
            ctx.globalAlpha = (180 - 150 * k / (sc.trail > 1 ? sc.trail - 1 : 1)) / 255;
            ctx.fillStyle = sc.color;
            ctx.fill();
            ctx.globalAlpha = 1;
        }

        ctx.beginPath();
        ctx.arc(C + pt.px, C + pt.py, GM_DOT_D / 2, 0, Math.PI * 2);
        ctx.fillStyle = sc.color;
        ctx.fill();
        ctx.lineWidth = 2;
        ctx.strokeStyle = '#ffffff';
        ctx.stroke();
    }

    // ---- дополнительные виджеты ----
    function textSize(ctx, w, text) {
        const px = fontPx(w.font);
        ctx.font = fontCss(w.font, px);
        return [Math.max(ctx.measureText(text).width, px * 0.6), px];
    }

    function widgetText(w, sig, v) {
        return w.type === 'text' ? w.text : (v == null ? '--' : (v / (w.div > 0 ? w.div : 1)).toFixed(Math.min(Math.max(sig.decimals | 0, 0), 10)));
    }

    // Рамка виджета на экране: [x, y, w, h] в пикселях canvas.
    function bounds(ctx, w, sig, v, imgs) {
        let bw, bh;
        if (w.type === 'value' || w.type === 'text') {
            [bw, bh] = textSize(ctx, w, widgetText(w, sig, v));
        } else if (w.type === 'image' || w.type === 'indicator') {
            const img = w.image ? imgs[w.image] : null;
            [bw, bh] = img ? [img.width, img.height] : (w.type === 'indicator' ? [w.w, w.w] : [60, 60]);
        } else if (w.type === 'bar') {
            [bw, bh] = [w.w, w.h];
        } else {
            [bw, bh] = [w.w, w.w];
        }
        return [C + w.x - bw / 2, C + w.y - bh / 2, bw, bh];
    }

    function drawWidget(ctx, layout, sc, w, values, imgs, editor) {
        const sig = widgetSig(layout, sc, w);
        const v = values[sig.name] ?? null;
        const cx = C + w.x, cy = C + w.y;
        const zone = (w.zone_color && v != null) ? zoneAt(sig.zones, v) : null;
        const color = zone ? zone.color : w.color;

        if (w.type === 'value' || w.type === 'text') {
            ctx.font = fontCss(w.font, fontPx(w.font));
            ctx.fillStyle = color;
            ctx.textAlign = 'center';
            ctx.textBaseline = 'middle';
            ctx.fillText(widgetText(w, sig, v), cx, cy);
        } else if (w.type === 'image') {
            const img = w.image ? imgs[w.image] : null;
            if (img) {
                ctx.drawImage(img, cx - img.width / 2, cy - img.height / 2);
            } else if (editor) {
                ctx.setLineDash([4, 4]);
                ctx.strokeStyle = GREY;
                ctx.lineWidth = 1;
                ctx.strokeRect(cx - 30, cy - 30, 60, 60);
                ctx.setLineDash([]);
            }
        } else if (w.type === 'indicator') {
            let on = v != null && (w.op === '<' ? v < w.threshold : v > w.threshold);
            if (on && w.blink) on = Math.floor(performance.now() / BLINK_HALF_MS) % 2 === 0;
            const img = w.image ? imgs[w.image] : null;
            if (on) {
                if (img) {
                    ctx.drawImage(img, cx - img.width / 2, cy - img.height / 2);
                } else {
                    ctx.beginPath();
                    ctx.arc(cx, cy, Math.max(0, w.w / 2), 0, Math.PI * 2);
                    ctx.fillStyle = w.color;
                    ctx.fill();
                }
            } else if (editor) {
                // Выключенный индикатор на плате невидим; в редакторе показываем контур,
                // чтобы его можно было найти и перетащить.
                ctx.setLineDash([3, 3]);
                ctx.strokeStyle = GREY;
                ctx.lineWidth = 1;
                ctx.beginPath();
                ctx.arc(cx, cy, Math.max(0, (img ? img.width : w.w) / 2), 0, Math.PI * 2);
                ctx.stroke();
                ctx.setLineDash([]);
            }
        } else if (w.type === 'bar') {
            const f = v == null ? 0 : frac(sig.min, sig.max, v);
            const x = cx - w.w / 2, y = cy - w.h / 2;
            const rr = (px, py, pw, ph, col) => {
                ctx.beginPath();
                ctx.roundRect(px, py, pw, ph, 3);
                ctx.fillStyle = col;
                ctx.fill();
            };
            rr(x, y, w.w, w.h, w.bg_color);
            if (f > 0) {
                if (w.w >= w.h) rr(x, y, w.w * f, w.h, color);
                else rr(x, y + w.h * (1 - f), w.w, w.h * f, color);
            }
        } else if (w.type === 'arc') {
            arcShape(ctx, cx, cy, w.w, w.width, w.angle, w.rotation, w.bg_color, color,
                v == null ? 0 : frac(sig.min, sig.max, v));
        }
    }

    // ---- экран целиком ----
    // values - имя сигнала -> число (нет ключа - нет данных); imgs - имя -> canvas;
    // sel - номер выделенного виджета (рамка) или -1; editor - рисовать подсказки;
    // extra - шлейф и максимумы G-сенсора (их ведёт вызывающий).
    function draw(canvas, layout, sc, values, imgs, sel, editor, extra) {
        const ctx = canvas.getContext('2d');
        ctx.save();
        ctx.clearRect(0, 0, SIZE, SIZE);
        ctx.beginPath();
        ctx.arc(C, C, SIZE / 2, 0, Math.PI * 2);
        ctx.clip();
        ctx.fillStyle = sc.bg_color;
        ctx.fillRect(0, 0, SIZE, SIZE);
        const bg = sc.bg_image ? imgs[sc.bg_image] : null;
        if (bg) ctx.drawImage(bg, (SIZE - bg.width) / 2, (SIZE - bg.height) / 2);

        const sig = mainSig(layout, sc);
        const v = values[sc.signal] ?? null;
        if (sc.type === 'dial') drawDial(ctx, sc, sig, v, imgs);
        else if (sc.type === 'ring') drawRing(ctx, sc, sig, v);
        else if (sc.type === 'gmeter') drawGmeter(ctx, sc, values, extra);

        // Один неудачный виджет (полуправленное поле) не должен прятать остальные.
        sc.widgets.forEach((w) => {
            try {
                drawWidget(ctx, layout, sc, w, values, imgs, editor);
            } catch (e) {
                console.warn('widget preview', w.type, e);
                ctx.setLineDash([]);
            }
        });

        if (editor && sel >= 0 && sc.widgets[sel]) {
            const w = sc.widgets[sel];
            const wsig = widgetSig(layout, sc, w);
            const [bx, by, bw, bh] = bounds(ctx, w, wsig, values[wsig.name] ?? null, imgs);
            ctx.setLineDash([6, 4]);
            ctx.lineWidth = 1.5;
            ctx.strokeStyle = '#00d0ff';
            ctx.strokeRect(bx - 3, by - 3, bw + 6, bh + 6);
            ctx.setLineDash([]);
        }
        ctx.restore();
    }

    // Верхний виджет под точкой (x, y в пикселях canvas) или -1.
    function hitTest(canvas, layout, sc, values, imgs, x, y) {
        const ctx = canvas.getContext('2d');
        for (let i = sc.widgets.length - 1; i >= 0; i--) {
            const w = sc.widgets[i];
            const sig = widgetSig(layout, sc, w);
            const [bx, by, bw, bh] = bounds(ctx, w, sig, values[sig.name] ?? null, imgs);
            const pad = 6;
            if (x >= bx - pad && x <= bx + bw + pad && y >= by - pad && y <= by + bh + pad) return i;
        }
        return -1;
    }

    return { draw, hitTest, sigDef, mainSig, gmeterPoint, gmeterTrack, fontMap, SIZE };
})();
