#!/usr/bin/env python3
"""Дополняет папку релиза до портативного набора для прошивки "на столе" и пакует её в zip.

Вызывается из build.bat / build.sh после того, как в папку релиза уже скопированы образы,
flash_args и README.txt:

    package_release.py <папка релиза> <версия>

Что делает:
  * кладёт в папку esptool.exe (официальная сборка с GitHub, скачивается один раз в
    tools/build/.cache и дальше берётся оттуда) и текст его лицензии;
  * пишет скрипты прошивки по flash_args: flash_all.bat / flash_all.sh (всё: загрузчик,
    таблица разделов, приложение, web, media) и update_firmware.bat / update_firmware.sh
    (только приложение и web - картинки и шрифты, загруженные в media, остаются);
  * собирает рядом с папкой zip: releases/<папка>.zip, внутри одна папка roundGauge_v<версия>.

Только стандартная библиотека Python.
"""
import os
import shutil
import sys
import urllib.request
import zipfile

# Версия закреплена, чтобы сборка не менялась сама: новый esptool - осознанное обновление.
ESPTOOL_TAG = "v5.4.0"
ESPTOOL_ASSET = "esptool-%s-windows-amd64.zip" % ESPTOOL_TAG
ESPTOOL_URL = "https://github.com/espressif/esptool/releases/download/%s/%s" % (ESPTOOL_TAG, ESPTOOL_ASSET)
BAUD = 460800

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, ".cache")


def log(msg):
    print("[package_release] " + msg)


def ensure_esptool():
    """Путь к esptool.exe и LICENSE в кэше; скачивает архив, если его ещё нет."""
    exe = os.path.join(CACHE, "esptool-" + ESPTOOL_TAG, "esptool.exe")
    lic = os.path.join(CACHE, "esptool-" + ESPTOOL_TAG, "LICENSE")
    if os.path.isfile(exe) and os.path.isfile(lic):
        return exe, lic
    os.makedirs(os.path.dirname(exe), exist_ok=True)
    archive = os.path.join(CACHE, ESPTOOL_ASSET)
    if not os.path.isfile(archive):
        log("downloading %s (once, cached in tools/build/.cache) ..." % ESPTOOL_URL)
        part = archive + ".part"
        with urllib.request.urlopen(ESPTOOL_URL) as resp, open(part, "wb") as out:
            shutil.copyfileobj(resp, out)
        os.replace(part, archive)
    with zipfile.ZipFile(archive) as z:
        for name in z.namelist():
            base = name.rsplit("/", 1)[-1]
            if base in ("esptool.exe", "LICENSE"):
                with z.open(name) as src, open(os.path.join(os.path.dirname(exe), base), "wb") as dst:
                    shutil.copyfileobj(src, dst)
    if not (os.path.isfile(exe) and os.path.isfile(lic)):
        raise SystemExit("esptool.exe / LICENSE not found in " + archive)
    return exe, lic


def read_flash_args(path):
    """(строка опций, [(смещение, файл), ...]) из flash_args папки релиза."""
    opts, entries = "", []
    with open(path, encoding="ascii") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("--"):
                opts = line
            else:
                off, name = line.split(None, 1)
                entries.append((off, name.strip()))
    if not opts or not entries:
        raise SystemExit("flash_args is empty or malformed: " + path)
    return opts, entries


def crlf(text):
    return text.replace("\r\n", "\n").replace("\n", "\r\n")


def bat(version, title, notes, esptool_args):
    return crlf("""@echo off
setlocal
cd /d "%~dp0"
REM roundGauge v{ver}: {title}
REM Optional argument: the serial port, e.g.  {name} COM5
set "PORTARG="
if not "%~1"=="" set "PORTARG=-p %~1"

echo roundGauge v{ver}: {title}
{notes}
echo.
esptool.exe --chip esp32s3 -b {baud} %PORTARG% --before default-reset --after hard-reset write-flash {args}
if errorlevel 1 (
    echo.
    echo *** FLASHING FAILED ***
    echo Check the USB cable and the port ^(try: %~nx0 COM5^), unplug the 12 V supply, and try again.
) else (
    echo.
    echo Done. The board restarts by itself.
)
pause
""".format(ver=version, title=title, notes=notes, baud=BAUD, args=esptool_args, name="{name}"))


def sh(version, title, esptool_args):
    return """#!/usr/bin/env bash
# roundGauge v{ver}: {title}
# Optional argument: the serial port, e.g.  ./{{name}} /dev/ttyUSB0
# Needs esptool:  pip install esptool
set -e
cd "$(dirname "$0")"
PORT_ARGS=()
if [ -n "${{1:-}}" ]; then PORT_ARGS=(-p "$1"); fi
if command -v esptool >/dev/null 2>&1; then ESPTOOL=(esptool)
elif command -v esptool.py >/dev/null 2>&1; then ESPTOOL=(esptool.py)
else ESPTOOL=(python3 -m esptool); fi
echo "roundGauge v{ver}: {title}"
"${{ESPTOOL[@]}}" --chip esp32s3 -b {baud} ${{PORT_ARGS[@]+"${{PORT_ARGS[@]}}"}} --before default-reset --after hard-reset write-flash {args}
echo "Done. The board restarts by itself."
""".format(ver=version, title=title, baud=BAUD, args=esptool_args)


def write_scripts(rel_dir, version, opts, entries):
    full_args = '"@flash_args"'
    part = [(o, n) for o, n in entries if n.startswith(("roundGauge_firmware_", "roundGauge_www_"))]
    if len(part) != 2:
        raise SystemExit("firmware/www entries not found in flash_args")
    part_args = opts + " " + " ".join("%s %s" % (o, n) for o, n in part)

    notes_all = "\n".join([
        "echo Connect the board with a Type-C cable. If the 12 V supply is connected, UNPLUG it first.",
        "echo Writes everything: bootloader, partitions, firmware, web interface, media.",
        "echo Settings, screen layout and passwords stored in NVS are kept;",
        "echo images and fonts in the media partition are replaced by the base set.",
    ])
    notes_upd = "\n".join([
        "echo Connect the board with a Type-C cable. If the 12 V supply is connected, UNPLUG it first.",
        "echo Writes only the firmware and the web interface.",
        "echo Settings, layout and the images/fonts you uploaded to media are kept.",
    ])
    files = {
        "flash_all.bat": bat(version, "full flash", notes_all, full_args).replace("{name}", "flash_all.bat"),
        "update_firmware.bat": bat(version, "firmware and web update", notes_upd, part_args).replace("{name}", "update_firmware.bat"),
        "flash_all.sh": sh(version, "full flash", full_args).replace("{name}", "flash_all.sh"),
        "update_firmware.sh": sh(version, "firmware and web update", part_args).replace("{name}", "update_firmware.sh"),
    }
    for name, text in files.items():
        with open(os.path.join(rel_dir, name), "w", encoding="ascii", newline="") as f:
            f.write(text)
    return list(files)


def make_zip(rel_dir, version):
    rel_dir = os.path.abspath(rel_dir)
    out = rel_dir + ".zip"
    top = "roundGauge_v" + version
    if os.path.exists(out):
        os.remove(out)
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for name in sorted(os.listdir(rel_dir)):
            path = os.path.join(rel_dir, name)
            if not os.path.isfile(path):
                continue
            info = zipfile.ZipInfo.from_file(path, "%s/%s" % (top, name))
            info.compress_type = zipfile.ZIP_DEFLATED
            # .sh и esptool.exe запускаемые: сохраняем бит исполнения для Linux/macOS
            mode = 0o755 if name.endswith((".sh", ".exe")) else 0o644
            info.external_attr = (mode | 0o100000) << 16
            with open(path, "rb") as f:
                z.writestr(info, f.read())
    return out


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: package_release.py <release dir> <version>")
    rel_dir, version = sys.argv[1], sys.argv[2]
    exe, lic = ensure_esptool()
    shutil.copy2(exe, os.path.join(rel_dir, "esptool.exe"))
    shutil.copy2(lic, os.path.join(rel_dir, "esptool_LICENSE.txt"))
    opts, entries = read_flash_args(os.path.join(rel_dir, "flash_args"))
    scripts = write_scripts(rel_dir, version, opts, entries)
    out = make_zip(rel_dir, version)
    log("scripts: " + ", ".join(scripts))
    log("zip: %s (%.1f MB)" % (out, os.path.getsize(out) / 1048576))


if __name__ == "__main__":
    main()
