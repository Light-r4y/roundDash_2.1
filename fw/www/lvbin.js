// Кодек картинок LVGL .bin (заголовок lv_image_header_t, 12 байт, little-endian):
//   [0] magic 0x19  [1] формат цвета  [2..3] флаги  [4..5] ширина  [6..7] высота
//   [8..9] stride (байт в строке)  [10..11] резерв
// Плата читает такие файлы встроенным декодером LVGL без разбора PNG, поэтому
// конвертация делается здесь, в браузере.
//   0x12 RGB565   - без прозрачности, 2 байта на пиксель (фоны)
//   0x10 ARGB8888 - с прозрачностью, 4 байта на пиксель, порядок B,G,R,A (стрелки)
const RgLvBin = (() => {
    const MAGIC = 0x19;
    const CF_ARGB8888 = 0x10, CF_XRGB8888 = 0x11, CF_RGB565 = 0x12;
    const MAX_SIDE = 480;

    function header(cf, w, h, stride) {
        const hd = new Uint8Array(12);
        const dv = new DataView(hd.buffer);
        hd[0] = MAGIC;
        hd[1] = cf;
        dv.setUint16(4, w, true);
        dv.setUint16(6, h, true);
        dv.setUint16(8, stride, true);
        return hd;
    }

    // ImageData -> .bin (ArrayBuffer). Прозрачные пиксели есть - ARGB8888, иначе RGB565.
    function encode(img) {
        const { width: w, height: h, data } = img;
        let alpha = false;
        for (let i = 3; i < data.length; i += 4) {
            if (data[i] < 255) { alpha = true; break; }
        }
        const bpp = alpha ? 4 : 2;
        const out = new Uint8Array(12 + w * h * bpp);
        out.set(header(alpha ? CF_ARGB8888 : CF_RGB565, w, h, w * bpp), 0);
        let o = 12;
        for (let i = 0; i < data.length; i += 4) {
            const r = data[i], g = data[i + 1], b = data[i + 2], a = data[i + 3];
            if (alpha) {
                out[o++] = b; out[o++] = g; out[o++] = r; out[o++] = a;
            } else {
                const v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                out[o++] = v & 0xFF; out[o++] = v >> 8;
            }
        }
        return { buf: out.buffer, w, h, alpha };
    }

    // .bin -> canvas (для предпросмотра). null - формат не поддержан.
    function decode(buf) {
        if (buf.byteLength < 12) return null;
        const u8 = new Uint8Array(buf);
        const dv = new DataView(buf);
        if (u8[0] !== MAGIC) return null;
        const cf = u8[1], w = dv.getUint16(4, true), h = dv.getUint16(6, true), stride = dv.getUint16(8, true);
        if (!w || !h || ![CF_ARGB8888, CF_XRGB8888, CF_RGB565].includes(cf)) return null;
        const bpp = cf === CF_RGB565 ? 2 : 4;
        if (stride < w * bpp || buf.byteLength < 12 + stride * h) return null;
        const canvas = document.createElement('canvas');
        canvas.width = w; canvas.height = h;
        const ctx = canvas.getContext('2d');
        const id = ctx.createImageData(w, h);
        for (let y = 0; y < h; y++) {
            let p = 12 + y * stride;
            for (let x = 0; x < w; x++) {
                const o = (y * w + x) * 4;
                if (cf === CF_RGB565) {
                    const v = u8[p] | (u8[p + 1] << 8);
                    id.data[o] = ((v >> 11) & 31) * 255 / 31;
                    id.data[o + 1] = ((v >> 5) & 63) * 255 / 63;
                    id.data[o + 2] = (v & 31) * 255 / 31;
                    id.data[o + 3] = 255;
                } else {
                    id.data[o] = u8[p + 2]; id.data[o + 1] = u8[p + 1]; id.data[o + 2] = u8[p];
                    id.data[o + 3] = cf === CF_ARGB8888 ? u8[p + 3] : 255;
                }
                p += bpp;
            }
        }
        ctx.putImageData(id, 0, 0);
        return canvas;
    }

    // Файл-картинка (PNG/JPG/WebP...) -> .bin; слишком большие уменьшаются до 480 px.
    async function fromImageFile(file) {
        const bmp = await createImageBitmap(file);
        const k = Math.min(1, MAX_SIDE / Math.max(bmp.width, bmp.height));
        const w = Math.max(1, Math.round(bmp.width * k)), h = Math.max(1, Math.round(bmp.height * k));
        const canvas = document.createElement('canvas');
        canvas.width = w; canvas.height = h;
        const ctx = canvas.getContext('2d');
        ctx.drawImage(bmp, 0, 0, w, h);
        bmp.close();
        return encode(ctx.getImageData(0, 0, w, h));
    }

    return { encode, decode, fromImageFile };
})();
