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

    const fontPx = (name) => (name === '14' ? 14 : name === '48' ? 48 : 28);

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
            const len = major ? 22 : 10;
            line(ctx, polar(R - len, ang), polar(R, ang), major ? 4 : 2, zone ? zone.color : (major ? sc.color : GREY));
        }
        const nlab = Math.floor((n - 1) / sc.major_every) + 1;
        for (let k = 0; k < nlab; k++) {
            const f = nlab > 1 ? (k * sc.major_every) / (n - 1) : 0;
            const zone = zoneAt(sig.zones, sig.min + (sig.max - sig.min) * f);
            const [x, y] = polar(R - 22 - 24, a0 + span * f);
            ctx.font = '28px ' + FONT;
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
            ctx.drawImage(img, 0, -img.height / 2);
            ctx.restore();
        } else {
            line(ctx, [C, C], polar(R - NEEDLE_MARGIN, ang), sc.needle_width, sc.needle_color, 'round');
        }
    }

    function arcShape(ctx, cx, cy, diameter, width, angle, rotation, bg, fg, f) {
        const a0 = rotation * RAD, span = angle * RAD, r = diameter / 2 - width / 2;
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

    // ---- дополнительные виджеты ----
    function textSize(ctx, w, text) {
        const px = fontPx(w.font);
        ctx.font = px + 'px ' + FONT;
        return [Math.max(ctx.measureText(text).width, px * 0.6), px];
    }

    function widgetText(w, sig, v) {
        return w.type === 'text' ? w.text : (v == null ? '--' : v.toFixed(sig.decimals));
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
            ctx.font = fontPx(w.font) + 'px ' + FONT;
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
                    ctx.arc(cx, cy, w.w / 2, 0, Math.PI * 2);
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
                ctx.arc(cx, cy, (img ? img.width : w.w) / 2, 0, Math.PI * 2);
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
    // sel - номер выделенного виджета (рамка) или -1; editor - рисовать подсказки.
    function draw(canvas, layout, sc, values, imgs, sel, editor) {
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

        sc.widgets.forEach((w) => drawWidget(ctx, layout, sc, w, values, imgs, editor));

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

    return { draw, hitTest, sigDef, mainSig, SIZE };
})();
