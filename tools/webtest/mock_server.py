#!/usr/bin/env python3
"""
Автономный мок-сервер для ручного тестирования веб-интерфейса roundGauge в
обычном браузере на компьютере - без прошивки, без ESP32, без CAN-железа.

Раздаёт статику прямо из ../../fw/www (то есть то же самое, что попадёт в образ
раздела www) и подменяет /api/* эндпоинты правдоподобными данными, которые
живут в памяти процесса на время его работы.

Запуск:
    python mock_server.py [порт] [--password ПАРОЛЬ]   (по умолчанию порт 8088)

Открыть в браузере:
    http://localhost:8088/        - главная страница (состояние, яркость, пароль)
    http://localhost:8088/ota     - обновление прошивки и www
    http://localhost:8088/editor.html - редактор экранов
    http://localhost:8088/can.html - настройки CAN, привязки, сниффер
    http://localhost:8088/imu.html - датчик: калибровка, направление, живые показания
    http://localhost:8088/access.html - пароль на настройки и Wi-Fi точки доступа

Важно:
    - Эндпоинты - те, что описаны в docs/fw-design.md §8 и которые уже ждут
      страницы: /api/status, /api/auth/status, /api/auth/verify,
      /api/ota/update, /api/www/update, /api/layout, /api/layout/reset, /api/media,
      /api/media/delete и /media/<файл>. Layout и картинки живут в памяти процесса.
    - --password включает защиту паролем, как если бы он был задан на плате:
      так проверяется кнопка-замочек и окно ввода пароля.
    - Страница /ota берётся из fw/tasks/webcfg/ota_page.h, а загрузка образа
      только имитируется: тело читается с задержкой и ответ всегда "success"
      (или ошибка записи при ?fail=1, оборванное соединение при ?hang=1,
      ответ, который так и не приходит, при ?stall=1, тот же образ при
      ?same=1). Ничего не записывается.
"""
import base64
import http.server
import json
import os
import random
import re
import sys
import threading
import time

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
WWW_DIR = os.path.join(ROOT, "fw", "www")
VERSION_FILE = os.path.join(ROOT, "fw", "version.txt")
OTA_PAGE_H = os.path.join(ROOT, "fw", "tasks", "webcfg", "ota_page.h")
LAYOUT_DEFAULT_H = os.path.join(ROOT, "fw", "common", "config", "layout_default.h")
CAN_MAP_DEFAULT_H = os.path.join(ROOT, "fw", "common", "config", "can_map_default.h")
LAYOUT_JSON_MAX = 16384   # RG_LAYOUT_JSON_MAX
UI_MAX_BG_IMAGES = 4      # RG_UI_MAX_BG_IMAGES
MEDIA_MAX_FILE = 1000000  # RG_MEDIA_MAX_FILE
MEDIA_TOTAL = 9 * 1024 * 1024
MEDIA_NAME_RE = re.compile(r"^(?!\.)[A-Za-z0-9._-]{1,31}$")
START_TIME = time.time()

try:
    with open(VERSION_FILE, encoding="utf-8") as f:
        FW_VERSION = f.read().strip()
except OSError:
    FW_VERSION = "0.0.0-mock"

def load_default_layout():
    """Встроенный layout - из того же layout_default.h, что компилируется в прошивку:
    склеиваем строковые литералы C."""
    text = open(LAYOUT_DEFAULT_H, encoding="utf-8").read()
    body = text[text.index("RG_LAYOUT_DEFAULT_JSON"):]
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', body)
    return "".join(parts).replace('\\"', '"')


def load_default_can_map():
    """Пресет rusEFI из того же can_map_default.h, что компилируется в прошивку."""
    with open(CAN_MAP_DEFAULT_H, encoding="utf-8") as f:
        text = f.read()
    pieces = re.findall(r'"((?:[^"\\]|\\.)*)"', text[text.index("RG_CAN_MAP_DEFAULT_JSON"):])
    return "".join(pieces).replace('\\"', '"')


lock = threading.Lock()

state = {
    "auth_password": "",
    "www_update": None,         # итог последнего обновления www, как в /api/status на плате
    "build_id_override": None,  # "новый" /.build_id после имитации обновления
    "layout": None,             # сохранённый layout (str); None - встроенный
    "media": {},                # имя -> байты, как раздел media на плате
    "can_cfg": {"bitrate": 500000, "mode": "listen_only", "demo": False},
    "can_map": None,  # сохранённая таблица привязок (str); None - действует пресет rusEFI
    "brightness": 100,
    "wifi": {"ssid": "", "password": "roundgauge"},
    "imu": {"calibrated": False, "fwd": 0, "cal": "idle", "cal_t": 0.0, "detect": "idle", "detect_t": 0.0},
    "sniff_t": 0.0,             # когда веб последний раз опрашивал сниффер
}


def layout_is_valid(text):
    """Как roundGauge_layout_apply_json: JSON разбирается и есть хотя бы один годный экран."""
    try:
        data = json.loads(text)
    except ValueError:
        return False
    screens = data.get("screens") if isinstance(data, dict) else None
    if not isinstance(screens, list):
        return False
    bg = {sc.get("bg_image") for sc in screens[:8] if isinstance(sc, dict) and sc.get("bg_image")}
    if len(bg) > UI_MAX_BG_IMAGES:
        return False
    for sc in screens:
        if (isinstance(sc, dict) and sc.get("type") in ("dial", "ring", "number") and sc.get("signal")
                and sc.get("max", 100) > sc.get("min", 0)):
            return True
    return False


def check_auth(handler):
    """Как в прошивке (common/config/auth.h): чтение всегда открыто, для
    изменяющих запросов - Basic Auth по паролю (логин игнорируется), если
    пароль задан."""
    if not state["auth_password"]:
        return True
    header = handler.headers.get("Authorization", "")
    if header.startswith("Basic "):
        try:
            decoded = base64.b64decode(header[6:]).decode("utf-8")
            _, _, password = decoded.partition(":")
            if password == state["auth_password"]:
                return True
        except Exception:
            pass
    handler.send_response(401)
    handler.send_header("WWW-Authenticate", 'Basic realm="roundGauge"')
    handler.send_header("Content-Length", "0")
    handler.end_headers()
    return False


CAN_MAP_MAX = 16
# Поддельная шина: id, ext, период (с), функция данных от времени t.
def _fake_bus():
    import struct
    def rpm(t):   # 0x201: обороты, big-endian 16 бит, цена 0,25
        v = int((3000 + 2500 * __import__("math").sin(t / 2.0)) * 4)
        return struct.pack(">H", v) + bytes([0x11, 0x22, 0, 0, 0, 0])
    def cool(t):  # 0x420: температура +40 в байте 0
        return bytes([int(85 + 25 * __import__("math").sin(t / 7.0)) + 40, 0, 0, 0, 0, 0, 0, 0])
    def speed(t): # 0x3B3: скорость, little-endian 16 бит, цена 0,01 км/ч
        return struct.pack("<H", int((90 + 80 * __import__("math").sin(t / 5.0)) * 100)) + bytes(6)
    def counter(t):
        return bytes([int(t * 10) & 0xFF, 0xAA, 0, 0, 0, 0, 0, int(t) & 0xFF])
    return [
        (0x201, False, 0.01, rpm), (0x420, False, 0.1, cool), (0x3B3, False, 0.02, speed),
        (0x7E8, False, 1.0, lambda t: bytes([3, 0x41, 0x0D, 0x40, 0, 0, 0, 0])),
        (0x18FEF100, True, 0.1, counter),
    ]


def fake_frames_payload():
    """Как GET /api/can/frames: пока веб не опрашивал сниффер дольше 3 с, плата чужие
    ID не принимает - первый ответ пустой."""
    now = time.time()
    warm = (now - state["sniff_t"]) < 3.0
    state["sniff_t"] = now
    if not warm:
        return []
    t = now - START_TIME
    out = []
    for fid, ext, period, fn in _fake_bus():
        data = fn(t)
        out.append({"id": fid, "ext": ext, "dlc": len(data), "data": data.hex(),
                    "count": int(t / period), "age": int((t % period) * 1000)})
    return out


def imu_payload():
    """Как GET /api/imu: плавное движение точки; направление "вперёд" поворачивает оси."""
    import math
    st = state["imu"]
    now = time.time()
    if st["cal"] == "running" and now - st["cal_t"] > 1.2:
        st["cal"], st["calibrated"] = "done", True
    if st["detect"] == "armed" and now - st["detect_t"] > 3.0:
        st["detect"], st["fwd"] = "done", (st["fwd"] + 1) % 4
    t = now - START_TIME
    lon0 = 0.9 * math.sin(t * 0.9) + 0.25 * math.sin(t * 2.3)
    lat0 = 0.8 * math.sin(t * 1.3 + 1.0)
    # Плата стоит так, что сила тяжести вдоль оси Y (g0 = 0,1,0); горизонтальны X и Z. Физическое ускорение
    # (dx, dz) не зависит от выбранного варианта, от него зависят только lon и lat (как в imu_math.c).
    dx, dz = lon0, lat0
    k = st["fwd"]
    lon, lat = [(dx, dz), (-dx, -dz), (dz, -dx), (-dz, dx)][k]
    return {"ok": True, "calibrated": st["calibrated"], "fwd": st["fwd"], "cal": st["cal"], "detect": st["detect"],
            "raw": [round(dx, 3), round(1 + 0.02 * math.sin(t), 3), round(dz, 3)], "g0": [0, 1, 0],
            "lon": round(lon, 3), "lat": round(lat, 3), "vert": round(0.05 * math.sin(t * 5), 3),
            "tot": round(math.hypot(lon, lat), 3)}


def can_status_payload():
    t = time.time() - START_TIME
    rx = int(sum(t / p for _, _, p, _ in _fake_bus()))
    return {**state["can_cfg"], "node_up": True, "state": 0, "rx": rx, "dropped": 0,
            "tx_err": 0, "rx_err": 0, "bus_err": 0, "busoff": 0}


def can_map_is_valid(text):
    """Как roundGauge_can_map_apply_json: JSON разбирается, map - массив, при непустом
    есть хотя бы одна годная запись."""
    try:
        data = json.loads(text)
    except ValueError:
        return False
    arr = data.get("map") if isinstance(data, dict) else None
    if not isinstance(arr, list):
        return False
    if not arr:
        return True
    for e in arr[:CAN_MAP_MAX]:
        try:
            if not isinstance(e, dict) or not e.get("signal"):
                continue
            eid = e["id"]
            eid = int(eid, 0) if isinstance(eid, str) else int(eid)
            if eid < 0 or (not e.get("ext") and eid > 0x7FF) or eid > 0x1FFFFFFF:
                continue
            if not (0 <= e["start"] <= 63 and 1 <= e["len"] <= 32):
                continue
            return True
        except (KeyError, ValueError, TypeError):
            continue
    return False


def build_status_payload():
    payload = {
        "fw_version": FW_VERSION,
        "uptime_s": int(time.time() - START_TIME),
        "reset_reason": 1,  # как esp_reset_reason(): 1 - включение питания
        "ap_clients": 1,
        "heap": {"int_free": 118000, "int_min": 92000, "int_block": 61000, "psram_free": 2400000, "psram_min": 2300000},
    }
    if state["www_update"]:
        payload["www_update"] = state["www_update"]
    return payload


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=WWW_DIR, **kwargs)

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def end_headers(self):
        # Без кэша: иначе после правки JS/CSS браузер держит старую версию, и
        # кажется, что код рассинхронизирован с ответами API.
        self.send_header("Cache-Control", "no-cache, no-store, must-revalidate")
        super().end_headers()

    def send_json(self, obj, status=200):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split("?")[0]
        if path == "/ota":
            return self.serve_ota_page()
        if path == "/.build_id" and state["build_id_override"]:
            # После имитации обновления файлов - "новый" хеш интерфейса, как на плате.
            body = state["build_id_override"].encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        if path.startswith("/media/"):
            return self.serve_media(path[len("/media/"):])
        if path.startswith("/api/"):
            return self.handle_api_get(path)
        return super().do_GET()

    def do_POST(self):
        path = self.path.split("?")[0]
        if path in ("/api/www/update", "/api/ota/update"):
            return self.api_fake_update()
        if path.startswith("/api/"):
            return self.handle_api_post(path)
        self.send_error(404)

    def serve_media(self, name):
        with lock:
            data = state["media"].get(name)
        if data is None:
            return self.send_error(404)
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def send_text(self, status, text):
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def query_name(self):
        m = re.search(r"[?&]name=([^&]+)", self.path)
        name = m.group(1) if m else ""
        if not MEDIA_NAME_RE.match(name):
            self.send_text(400, "Bad file name")
            return None
        return name

    def read_body(self, limit):
        length = int(self.headers.get("Content-Length", 0))
        if length <= 0 or length > limit:
            self.send_text(400, "Bad size")
            return None
        return self.rfile.read(length)

    def serve_ota_page(self):
        """Страница /ota в прошивке не лежит в www, а вкомпилирована из
        fw/tasks/webcfg/ota_page.h - берём её оттуда же, чтобы проверялся
        настоящий код."""
        text = open(OTA_PAGE_H, encoding="utf-8").read()
        start = text.index('R"rawliteral(') + len('R"rawliteral(')
        body = text[start:text.index(')rawliteral"')].encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def api_fake_update(self):
        """Имитация загрузки образа: тело читается порциями с задержкой, чтобы на
        странице был виден прогресс, как при медленном Wi-Fi платы. Ничего не
        записывается."""
        if not check_auth(self):
            return
        length = int(self.headers.get("Content-Length", 0))
        left = length
        while left > 0:
            chunk = self.rfile.read(min(left, 64 * 1024))
            if not chunk:
                break
            left -= len(chunk)
            time.sleep(0.03)
        m = re.search(r"[?&]id=([^&]+)", self.path)
        upd_id = m.group(1) if m else ""
        failed = left > 0 or "fail=1" in self.path
        if not failed and "same=1" not in self.path:
            # Файлы "записаны" - хеш интерфейса на плате меняется (см. /.build_id).
            state["build_id_override"] = "%064x" % random.getrandbits(256)
        blocks = (length + 65535) // 65536
        written = 0 if "same=1" in self.path else blocks // 3
        if upd_id:
            # Итог фиксируется до отправки ответа - его можно спросить через /api/status.
            state["www_update"] = {
                "id": upd_id, "done": True, "ok": not failed,
                **({"error": "Write failed"} if failed else {}),
                "blocks_written": written, "blocks_skipped": blocks - written, "total_ms": 4200,
            }
        if "stall=1" in self.path:
            # Файлы записаны, но ответ так и не приходит, а соединение висит.
            time.sleep(60)
            self.close_connection = True
            return
        if "hang=1" in self.path:
            # Тело принято, но ответ не уходит, а соединение закрывается.
            self.close_connection = True
            return
        if failed:
            body = b"Write failed"
            self.send_response(500)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        return self.send_json({"status": "success", "bytes": length,
                               "blocks_written": written, "blocks_skipped": blocks - written,
                               "flash_ms": 1800, "total_ms": 4200})

    def handle_api_get(self, path):
        with lock:
            if path == "/api/status":
                return self.send_json(build_status_payload())
            if path == "/api/auth/status":
                return self.send_json({"enabled": bool(state["auth_password"])})
            if path == "/api/layout":
                text = state["layout"] if state["layout"] is not None else load_default_layout()
                body = text.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path == "/api/imu":
                return self.send_json(imu_payload())
            if path == "/api/display":
                return self.send_json({"brightness": state["brightness"]})
            if path == "/api/wifi":
                w = state["wifi"]
                return self.send_json({"ssid": w["ssid"] or "roundGauge-A1B2", "default_ssid": "roundGauge-A1B2",
                                       "custom_ssid": bool(w["ssid"]),
                                       "password_default": w["password"] == "roundgauge"})
            if path == "/api/can":
                return self.send_json(can_status_payload())
            if path == "/api/can/map":
                body = (state["can_map"] if state["can_map"] is not None else load_default_can_map()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            if path == "/api/can/frames":
                return self.send_json(fake_frames_payload())
            if path == "/api/media":
                used = sum(len(v) for v in state["media"].values())
                return self.send_json({
                    "total": MEDIA_TOTAL, "used": used, "max_file": MEDIA_MAX_FILE,
                    "files": [{"name": n, "size": len(v)} for n, v in sorted(state["media"].items())]})
        self.send_error(404)

    def handle_api_post(self, path):
        with lock:
            if path == "/api/auth/verify":
                if not check_auth(self):
                    return
                return self.send_json({"status": "ok"})
            if path == "/api/layout":
                if not check_auth(self):
                    return
                body = self.read_body(LAYOUT_JSON_MAX)
                if body is None:
                    return
                text = body.decode("utf-8", "replace")
                if not layout_is_valid(text):
                    return self.send_text(400, "Invalid layout")
                state["layout"] = text
                return self.send_json({"status": "success"})
            if path == "/api/layout/reset":
                if not check_auth(self):
                    return
                state["layout"] = None
                return self.send_json({"status": "success"})
            if path == "/api/media":
                if not check_auth(self):
                    return
                name = self.query_name()
                if name is None:
                    return
                body = self.read_body(MEDIA_MAX_FILE)
                if body is None:
                    return
                used = sum(len(v) for k, v in state["media"].items() if k != name)
                if MEDIA_TOTAL - used < len(body) + len(body) // 8 + 32768:
                    return self.send_text(507, "Not enough space")
                state["media"][name] = body
                return self.send_json({"status": "success"})
            if path == "/api/imu/calibrate":
                if not check_auth(self):
                    return
                state["imu"].update(cal="running", cal_t=time.time())
                return self.send_json({"status": "started"})
            if path == "/api/imu/forward":
                if not check_auth(self):
                    return
                body = self.read_body(128)
                if body is None:
                    return
                try:
                    req = json.loads(body.decode("utf-8", "replace"))
                except ValueError:
                    return self.send_text(400, "Invalid JSON")
                if isinstance(req.get("fwd"), int) and 0 <= req["fwd"] <= 3:
                    state["imu"]["fwd"] = req["fwd"]
                elif req.get("detect") is True:
                    state["imu"].update(detect="armed", detect_t=time.time())
                else:
                    return self.send_text(400, "Expected fwd 0..3 or detect")
                return self.send_json({"status": "success"})
            if path == "/api/display":
                if not check_auth(self):
                    return
                body = self.read_body(128)
                if body is None:
                    return
                try:
                    b = json.loads(body.decode("utf-8", "replace")).get("brightness")
                except ValueError:
                    b = None
                if not isinstance(b, (int, float)) or not 5 <= b <= 100:
                    return self.send_text(400, "Brightness must be 5..100")
                state["brightness"] = int(b)
                return self.send_json({"status": "success"})
            if path == "/api/wifi":
                # Как на плате: Wi-Fi меняется только при заданном пароле на настройки.
                if not state["auth_password"]:
                    return self.send_text(403, "Set a settings password first")
                if not check_auth(self):
                    return
                body = self.read_body(512)
                if body is None:
                    return
                try:
                    data = json.loads(body.decode("utf-8", "replace"))
                except ValueError:
                    data = None
                ssid = data.get("ssid", "") if isinstance(data, dict) else None
                pw = data.get("password") if isinstance(data, dict) else None
                if not isinstance(ssid, str) or not isinstance(pw, str):
                    return self.send_text(400, "Invalid body")
                if len(ssid.encode("utf-8")) > 32 or not 8 <= len(pw) <= 64 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
                    return self.send_text(400, "SSID up to 32 bytes, password 8..64 characters")
                state["wifi"] = {"ssid": ssid, "password": pw}
                return self.send_json({"status": "success", "restart": True})
            if path == "/api/auth/password":
                if not check_auth(self):  # пока пароль есть - нужен текущий, как в прошивке
                    return
                body = self.read_body(256)
                if body is None:
                    return
                try:
                    pw = json.loads(body.decode("utf-8", "replace")).get("password")
                except ValueError:
                    pw = None
                if not isinstance(pw, str):
                    return self.send_text(400, "Invalid body")
                if pw and not 4 <= len(pw) <= 64:
                    return self.send_text(400, "Password must be 4..64 characters")
                state["auth_password"] = pw
                return self.send_json({"status": "success", "enabled": bool(pw)})
            if path == "/api/can":
                if not check_auth(self):
                    return
                body = self.read_body(256)
                if body is None:
                    return
                try:
                    req = json.loads(body.decode("utf-8", "replace"))
                except ValueError:
                    return self.send_text(400, "Invalid JSON")
                cfg = state["can_cfg"]
                if "bitrate" in req:
                    if not isinstance(req["bitrate"], (int, float)) or not 10000 <= req["bitrate"] <= 1000000:
                        return self.send_text(400, "Bitrate must be 10000..1000000")
                    cfg["bitrate"] = int(req["bitrate"])
                if "mode" in req:
                    if req["mode"] not in ("listen_only", "normal"):
                        return self.send_text(400, "Unknown mode")
                    cfg["mode"] = req["mode"]
                if "demo" in req and isinstance(req["demo"], bool):
                    cfg["demo"] = req["demo"]
                return self.send_json({"status": "success"})
            if path == "/api/can/map":
                if not check_auth(self):
                    return
                body = self.read_body(4096)
                if body is None:
                    return
                text = body.decode("utf-8", "replace")
                if not can_map_is_valid(text):
                    return self.send_text(400, "Invalid CAN map")
                state["can_map"] = text
                return self.send_json({"status": "success"})
            if path == "/api/can/map/reset":
                if not check_auth(self):
                    return
                state["can_map"] = None
                return self.send_json({"status": "success"})
            if path == "/api/media/delete":
                if not check_auth(self):
                    return
                name = self.query_name()
                if name is None:
                    return
                if state["media"].pop(name, None) is None:
                    return self.send_error(404)
                return self.send_json({"status": "success"})
        self.send_error(404)


Handler.extensions_map.update({
    ".woff2": "font/woff2",
    ".js": "text/javascript",
})


def main():
    args = sys.argv[1:]
    if "--password" in args:
        i = args.index("--password")
        if i + 1 >= len(args):
            sys.exit("--password требует значение")
        state["auth_password"] = args[i + 1]
        del args[i:i + 2]
    port = int(args[0]) if args else 8088

    if not os.path.isdir(WWW_DIR):
        sys.exit("fw/www не найдена по пути: %s" % WWW_DIR)
    with http.server.ThreadingHTTPServer(("0.0.0.0", port), Handler) as httpd:
        print("roundGauge mock server: http://localhost:%d/" % port)
        print("                        http://localhost:%d/ota" % port)
        print("Раздаёт файлы из: %s" % WWW_DIR)
        if state["auth_password"]:
            print("Защита паролем включена")
        print("Ctrl+C для остановки")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
