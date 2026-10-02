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
    http://localhost:8088/        - главная страница
    http://localhost:8088/ota     - обновление прошивки и www

Важно:
    - Эндпоинты - те, что описаны в docs/fw-design.md §6 и которые уже ждут
      страницы: /api/status, /api/auth/status, /api/auth/verify,
      /api/ota/update, /api/www/update. В прошивке они пока не реализованы.
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
START_TIME = time.time()

try:
    with open(VERSION_FILE, encoding="utf-8") as f:
        FW_VERSION = f.read().strip()
except OSError:
    FW_VERSION = "0.0.0-mock"

lock = threading.Lock()

state = {
    "auth_password": "",
    "www_update": None,         # итог последнего обновления www, как в /api/status на плате
    "build_id_override": None,  # "новый" /.build_id после имитации обновления
}


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


def build_status_payload():
    payload = {
        "fw_version": FW_VERSION,
        "uptime_s": int(time.time() - START_TIME),
        "reset_reason": "poweron",
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
        self.send_error(404)

    def handle_api_post(self, path):
        with lock:
            if path == "/api/auth/verify":
                if not check_auth(self):
                    return
                return self.send_json({"status": "ok"})
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
