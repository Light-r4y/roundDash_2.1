# roundGauge — CAN dashboard on ESP32-S3

[Русский](README.md) | **English**

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.0-red)](https://github.com/espressif/esp-idf)
[![GUI](https://img.shields.io/badge/GUI-LVGL%209-orange)](https://lvgl.io)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Firmware for a car dashboard on the round touch screen
[Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1)
(480×480): data comes from the CAN bus, what to show and how is configured
from a web page over Wi-Fi, and two physical buttons sit on the board's
connector.

> 🚧 **Status: skeleton.** The screen and touch work — after flashing, the
> screen shows a test gauge. CAN, Wi-Fi and web configuration are stubs with
> `TODO`s. Design decisions and open questions are in
> [docs/fw-design.md](docs/fw-design.md) (in Russian).

## ✨ Features

| | What | Status |
|---|---|---|
| 🖥️ | ST7701S 480×480 screen, LVGL 9, two frame buffers, no tearing | ✅ works |
| 👆 | CST820 touch | ✅ works |
| 🚌 | CAN (TWAI) reception, signal decoding from a table in the settings | ⏳ stub |
| 🎛️ | CAN mode: listen-only (default) or normal — for OBD-PID requests | ⏳ stub |
| 📶 | Wi-Fi access point on a long press of button 1 | ⏳ stub |
| 🌐 | Web configuration: light/dark theme, RU/EN, password for changes | ⏳ header only |
| 🔄 | Firmware and web interface updates via `/ota`, rollback of a failed update | ⏳ page exists, no server yet |
| 🖼️ | Dashboard backgrounds and needles from the `media` partition, custom uploads via the web | ⏳ stub |

## 🔧 Hardware

- **Board** Waveshare ESP32-S3-Touch-LCD-2.1: ESP32-S3R8, 16 MB flash, 8 MB PSRAM.
  Schematic and datasheets are in [hw/](hw/README.md) (in Russian).
- **CAN transceiver**, 3.3 V (SN65HVD230 or TJA1051T/3) — the board has none.
- **Power** in the car — a 12→5 V DC-DC converter with surge protection.

Everything connects to the J9 "12PIN Multi-function Interface" connector:

| J9 pin | Signal | Purpose |
|---|---|---|
| 2 | 5V | power from the DC-DC |
| 1, 5, 11 | GND | |
| 6 | 3V3 | transceiver supply |
| 3 | GPIO19 | CAN RX |
| 4 | GPIO20 | CAN TX |
| 12 | GPIO0 | button 1 (in parallel with BOOT) |
| 10 | GPIO44 | button 2 |

> ⚠️ In the car, power the board through J9 pin 2, not through the Type-C
> port: while Type-C is powered, a switch on the board hands GPIO44 to the
> USB-UART chip and button 2 stops working. Details:
> [docs/fw-design.md §3](docs/fw-design.md).

## 📁 Layout

```
├── fw/                 firmware (ESP-IDF project)
│   ├── main/           initialization and task startup
│   ├── tasks/          one component per task: can, ui, buttons, webcfg
│   ├── common/         board (pins, screen, touch), config (settings, password)
│   ├── www/            web interface → www partition
│   └── media/          dashboard images → media partition
├── docs/fw-design.md   firmware design, decisions, open questions
├── hw/                 board schematic, datasheets, mechanical drawings
└── tools/
    ├── build/          release build
    └── webtest/        mock server for the web interface
```

## 📋 Requirements

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/)
- Waveshare ESP32-S3-Touch-LCD-2.1 board, USB Type-C cable

Dependencies (LVGL, screen and touch drivers) are downloaded by the component
manager on the first build.

## 🚀 Quick start

```bash
cd fw
idf.py build
idf.py -p COM5 flash monitor
```

Use the port of the CH343 USB-UART on the board's Type-C; download mode is
entered automatically. The target (`esp32s3`), 16 MB flash, PSRAM and the
partition table are already set in `fw/sdkconfig.defaults`.

After flashing, the screen shows a round gauge whose needle is swept by an
animation, with the firmware version in the middle; a dot under your finger
shows that touch works.

> 💡 **Windows:** if ESP-IDF was installed with EIM and the system Python is
> newer than 3.11, `export.ps1` won't find its environment. Put the Python
> bundled with IDF first in `PATH` before running it:
> ```powershell
> $env:PATH = "$env:USERPROFILE\.espressif\tools\idf-python\3.11.2;$env:PATH"; . $env:USERPROFILE\esp\v6.0\esp-idf\export.ps1
> ```

## 📦 Release build

```bash
tools/build/build.sh        # Linux/macOS
tools\build\build.bat       # Windows
```

The script activates ESP-IDF by itself if needed, writes the version
`0.<YY>.<MMDD>` to `fw/version.txt`, builds with WARN log level and puts a
complete kit into `releases/roundGauge_v<version>_<time>/`: the firmware, the
`www` and `media` images, bootloader, partition table, `ota_data_initial.bin`
and `flash_args`. A new board is flashed from that folder with one command:

```bash
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
```

## 🌐 Web interface without hardware

```bash
python tools/webtest/mock_server.py
```

Open `http://localhost:8088/`. The server serves `fw/www` as is and emulates
the board's API — see [tools/webtest/README.md](tools/webtest/README.md)
(in Russian).

## 🧩 Third-party code

| What | Where | License |
|---|---|---|
| ST7701 init sequence and timings by Waveshare from [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) | `fw/common/board/st7701_init.h` | Apache-2.0 |
| [Alpine.js](https://alpinejs.dev) 3.14.1 | `fw/www/alpine.min.js` | MIT |
| [Bootstrap](https://getbootstrap.com) 5.3.0 | `fw/www/bootstrap.min.css` | MIT |
| [Font Awesome Free](https://fontawesome.com) 6.4.0 | `fw/www/fontawesome/` | icons CC BY 4.0, font SIL OFL 1.1, code MIT |

LVGL (MIT) and Espressif components (Apache-2.0) are downloaded at build time
and are not stored in the repository.

## 📄 License

[MIT](LICENSE)
