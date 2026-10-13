# roundGauge — firmware

[Русский](fw-design_ru.md)

A dashboard on the Waveshare ESP32-S3-Touch-LCD-2.1: data over CAN, configuration over Wi-Fi,
two buttons on the board connector and touch. ESP-IDF v6.0, target `esp32s3`, LVGL 9.6.

Status: working prototype.

## 1. Project structure

```
fw/
├── CMakeLists.txt          EXTRA_COMPONENT_DIRS = tasks, common; www and media images; build defines
├── sdkconfig.defaults
├── partitions.csv
├── version.txt             application version, written by tools/build/build.*
├── main/
│   └── main.c              initialization, starting the tasks, nothing else
├── tasks/                  every FreeRTOS task is a separate component
│   ├── can/                can_task: TWAI reception, parsing by the mapping table, sniffer
│   ├── ui/                 ui_task and ui_screens: LVGL, screens and widgets, overlays
│   ├── buttons/            buttons_task: polling the two buttons
│   ├── touch/              touch_task: CST820 touch, swipes
│   ├── imu/                imu_task: QMI8658 accelerometer, orientation, g_* signals
│   ├── sound/              sound_task: buzzer, alerts by rules, button clicks
│   ├── logger/             logger_task: recording signals to microSD in the MLG format (MegaLogViewer)
│   └── webcfg/             webcfg_task: access point, HTTP server, OTA
├── common/
│   ├── board/              pin map, I2C, TCA9554, ST7701 display, touch, backlight
│   ├── config/             conf.h, settings, web password, screen layout, CAN table
│   └── signals/            signal value store, timeouts, the "demo" generator
├── www/                    → www partition   (web interface)
└── media/                  → media partition (base set: backgrounds, needles, fonts)
```

Conventions:

- the files of a task `<x>_task.c/.h` sit together, it is started with `roundGauge_<x>_task_start()`;
- functions and types have the prefix `roundGauge_`, macros have `RG_`;
- stack, priority and core of all tasks are in `common/config/conf.h`;
- **GPIO numbers appear only in `common/board/board.h`.**

The files `sdkconfig`, `build/`, `managed_components/` and `dependencies.lock`
are generated and are not in the repository.

## 2. Tasks

| Task | Core | Priority | Stack | Period | Role |
|---|---|---|---|---|---|
| esp_lvgl_port | 1 | 4 | 8 K | — | `lv_timer_handler`, rendering |
| `ui_task` | 1 | 3 | 8 K | 16 ms | signals → widgets under `lvgl_port_lock()`, screen switching, overlays |
| `can_task` | 0 | 8 | 4 K | per frame | parsing frames from the queue, sniffer, bus-off |
| `touch_task` | 0 | 6 | 3 K | on interrupt | CST820 touch, swipe recognition |
| `imu_task` | 0 | 4 | 3 K | 8 ms | QMI8658 accelerometer → signals `g_lon`, `g_lat`, `g_vert`, `g_tot` |
| `sound_task` | 0 | 3 | 3 K | 10 ms | buzzer: alert rules, button clicks, test beep |
| `logger_task` | 0 | 2 | 8 K | 50 ms | recording logs to microSD: start conditions, file `log_NNNN.mlg` |
| `buttons_task` | 0 | 5 | 2.5 K | 10 ms | polling the buttons |
| `webcfg_task` | 0 | 2 | 4 K | 100 ms | bringing up the access point on request |
| httpd | 0 | 5 | 8 K | on request | created by esp_http_server |

Core 1 is rendering (LVGL rendering is single-threaded). Core 0 is CAN, touch, buttons, the sensor and Wi-Fi;
Wi-Fi is turned on only by a button.

`CONFIG_FREERTOS_HZ=1000`: even periods of 10 and 16 ms.

The path of a CAN frame:

```
TWAI ISR (on_rx_done) ──ID filter──xQueueSendFromISR──> queue (64) ──> can_task
                                                                          ├─> signal values ──> ui_task
                                                                          └─> sniffer ──> web
```

The ISR only copies the needed frame into the queue: the lookup in the mapping table and the
float arithmetic are done in `can_task`.

## 3. Hardware and pinout

All external signals are on the J9 connector "12PIN Multi-function Interface":

| J9 pin | Signal | Purpose |
|---|---|---|
| 2 | 5V | power from a 12→5 V DC-DC |
| 1, 5, 11 | GND | |
| 6 | 3V3 | transceiver power |
| 3 | GPIO19 | CAN RX |
| 4 | GPIO20 | CAN TX |
| 12 | GPIO0 | button 1 (in parallel with BOOT) |
| 10 | GPIO44 | button 2 |
| 9 | GPIO43 | logs (UART0 TX) |
| 7, 8 | GPIO7/15 | board I2C, spare |

- **CAN** needs an external 3.3 V transceiver (SN65HVD230 or TJA1051T/3). When connecting to a
  factory bus, remove the 120 Ω terminator on the transceiver module.
- **Power through J9 pin 2, not through Type-C.** The FSUSB42 switch hands GPIO43/44 to the CH343
  chip while Type-C has VBUS, and button 2 is disconnected from the connector in that time. On the
  desk, with Type-C power, only button 1 works.
- **Button 1 is on GPIO0**, a strapping pin: held at power-on, it puts the board into download
  mode. So everything done by holding button 1 is done on a running board (see §8).
- **USB-Serial/JTAG is off** (`CONFIG_ESP_CONSOLE_SECONDARY_NONE`) — its pins are given to CAN.
  The console is output only: UART0 RX is taken by button 2.
- The buttons close to GND; the GPIO44 line needs an external 10 kΩ to 3V3 and 100 nF to GND
  (GPIO0 already has a pull-up on the board).

The full pin map of the board (display, touch, TCA9554, I2C addresses) is in
`common/board/board.h`. The schematic and datasheets of the board chips are in `hw/`
([hw/README.md](../hw/README.md)).

### 3.1. Display and touch

The ST7701S init sequence, timings and reset order are taken from Waveshare's configuration of
this board in the library
[ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel/blob/master/src/board/supported/waveshare/BOARD_WAVESHARE_ESP32_S3_TOUCH_LCD_2_1.h)
(Apache-2.0, moved into `common/board/st7701_init.h` with the source credited).

| Parameter | Value |
|---|---|
| PCLK | 16 MHz (about 60 Hz panel refresh) |
| HSYNC: pulse / back / front | 8 / 10 / 50 |
| VSYNC: pulse / back / front | 3 / 8 / 8 |
| Bus | RGB565, 16 lines |
| Frame buffers | 2 × 450 KB in PSRAM |
| Bounce buffer | 480 × 10 pixels in internal RAM |

The order in `roundGauge_board_init()`:

1. backlight (LEDC on GPIO6, 5 kHz) — straight to 0, so garbage does not flash;
2. buttons, I2C 400 kHz on GPIO7/15, TCA9554 (0x20): only LCD_RST, TP_RST and LCD_CS are outputs;
3. display reset through TCA9554 (10 ms low, 100 ms high), 3-wire SPI
   (SCL/SDA on GPIO2/1, CS on TCA9554), `esp_lcd_new_panel_st7701()` → reset → init;
4. touch reset (30 / 50 ms), CST820 through the CST816S driver, interrupt on GPIO16
   (only with `RG_HAS_TOUCH=1`, see §10).

`ui_task` brings up LVGL through esp_lvgl_port in the **tear-free** mode: LVGL draws straight into
the panel frame buffers (direct mode), the buffers are swapped at the end of a frame. The backlight
turns on 100 ms after the first frame, the brightness is taken from the settings
(`display.brightness`).

**Touch is not registered in LVGL.** It is read by `touch_task` (`esp_lcd_touch`, on the INT
interrupt; while a finger is on the screen also every 20 ms, because the interrupt may not come
when the finger is lifted). The LVGL indev is not used for this: it reads touch only between
frames, and with a 25–45 ms frame swipes were lost. `touch_task` counts a swipe by the full
displacement from the touch point: at least 60 px horizontally, the horizontal displacement clearly
greater than the vertical one, within 800 ms; it fires at once, without waiting for the finger to
lift. Finger to the left — next screen, to the right — previous. The CST820 falls asleep without
touch and does not answer over I2C: a read error is treated as "no touch".

Settings for the RGB display in `sdkconfig.defaults`: CPU 240 MHz, PSRAM 80 MHz, code execution
from PSRAM (`SPIRAM_XIP_FROM_PSRAM` — writing to flash does not stop the frame output), data cache
line of 64 bytes, RGB interrupts in IRAM, DMA restart on every VSYNC, `-O2` optimization.

### 3.2. Rendering performance

While `RG_UI_SHOW_FPS` is on (1 by default in a normal build, 0 in a release build, §10), the top
of the screen has two lines: `2/3  38 fps` and `r 14  w 10 ms  25 Kpx`:

- `r` — the time of drawing a frame up to the flush of the last area;
- `w` — the wait in flush: in our mode `esp_lvgl_port` waits for the panel vsync (the
  `RENDER_READY` event comes only after it), this is not processor work;
- `Kpx` — how many pixels were redrawn in a frame (the sum of the flush areas).

FPS is limited by the sum `r + w`. Measurements on the board (dial, line needle, picture
background): `r` 14–18 ms, `w` 8–10 ms, 24–25 Kpx, 29–39 fps. What FPS depends on:

- **`LV_BIN_DECODER_RAM_LOAD=y` and a 3 MB image cache.** Without them `.bin` files are read from
  SPIFFS in pieces on every redraw of an area: a background with a needle gave 5 fps.
- **The line needle is computed by hand** (`needle_line_set` in `ui_screens.c`): a bounding
  rectangle of ~25 Kpx instead of ~150 Kpx with `lv_scale_set_line_needle_value`.
- **`LV_DEF_REFR_PERIOD=16` and a 16 ms `ui_task` period.** With the default (33) the ceiling is 30 fps.
- **`LV_USE_CLIB_MALLOC`.** The default LVGL pool is 64 KB; a 480×480 background (up to 922 KB)
  does not fit, `malloc` takes large blocks from PSRAM.
- **LVGL's built-in `sprintf` does not support `%f`:** floating-point numbers are formatted with
  libc `snprintf`.
- **Image cache warm-up** (§6): the first show of a screen without reading from flash.

Not implemented: the static part of the screen (background, scale, labels) as a single snapshot
(`lv_snapshot`).

## 4. Flash partitions

16 MB, two OTA areas with rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`;
the firmware confirms itself, `esp_ota_mark_app_valid_cancel_rollback()` in
`main.c` after the tasks are started):

```
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x10000
otadata,  data, ota,     0x19000,  0x2000
phy_init, data, phy,     0x1B000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x300000
ota_1,    app,  ota_1,   0x320000, 0x300000
www,      data, spiffs,  0x620000, 0x100000
media,    data, spiffs,  0x720000, 0x8E0000
```

## 5. Storage

| What | Where | How |
|---|---|---|
| Web interface | `www` partition, `/www` | an image from `fw/www/`, updated as a whole (`/ota`) |
| Images and fonts | `media` partition, `/media` | the base set from `fw/media/` (backgrounds `bg_*.bin`, needles `needle_*.bin` 199×44 with the pivot at 34,22, fonts `*.fnt`) + upload through the web, per file |
| General settings (CAN, brightness, Wi-Fi) | NVS `rg_cfg` / `cfg` | JSON blob |
| Screen layout | NVS `rg_cfg` / `layout` | JSON blob, up to 16 KB (§6) |
| Logs | microSD, FAT32 | files `log_NNNN.mlg` in the card root (§8, "Recording logs to SD"); start conditions are the `log` settings section |
| Sound alert rules | NVS `rg_cfg` / `alerts` | JSON blob, up to 2 KB, up to 8 rules (§8, "Sound"); until saved, the built-in ones apply |
| CAN mapping table | NVS `rg_cfg` / `can_map` | JSON blob, up to 4 KB (§7); until saved, the rusEFI preset applies |
| Web password | NVS `rg_cfg` / `auth_hash` | SHA-256(salt + password) |

Both SPIFFS images are built with `FLASH_IN_PROJECT`: **`idf.py flash` returns media to the base
set and erases files uploaded through the web.** NVS is not touched: the layout, the CAN table,
settings and the password are kept.

Settings in JSON: adding fields does not reset them, missing fields are taken from the defaults.
The sections `can`, `display`, `sound` and `log` apply on the fly
(`roundGauge_settings_set_can/_display/_sound/_log`), `wifi_ap` after the board restarts.

```json
{
  "version": 1,
  "can":     { "bitrate": 500000, "mode": "listen_only", "demo": false },
  "display": { "brightness": 100 },
  "sound":   { "mode": "off" },
  "log":     { "mode": "manual", "signal": "rpm", "op": ">", "value": 500, "stop_delay": 10 },
  "imu":     { "calibrated": false, "g0": [0, 0, 1], "fwd": 0 },
  "wifi_ap": { "ssid": "", "password": "roundgauge" }
}
```

| Field | Values | Default |
|---|---|---|
| `can.bitrate` | bit/s, 10000–1000000 | 500000 |
| `can.mode` | `listen_only` \| `normal` | `listen_only` — neither ACK nor frames; `normal` only acknowledges frames (ACK), the dashboard sends no frames of its own in either mode |
| `can.demo` | `true` \| `false` | `false` — values only from the bus; `true` — signals are driven by the generator (except the accelerometer `g_*`); for tuning the look on the desk |
| `display.brightness` | 5–100 | `100` — backlight brightness |
| `sound.mode` | `off`, `alerts`, `alerts_clicks` | `off` — the buzzer is silent; `alerts` — alerts by the rules; `alerts_clicks` — also a beep on button presses and swipes |
| `log.mode` | `manual`, `boot`, `condition` | `manual`; details in §8, "Recording logs to SD" |
| `log.signal`, `log.op`, `log.value`, `log.stop_delay` | a layout signal name, `>` or `<`, a number, 0–3600 s | `rpm`, `>`, `500`, `10`; used in the `condition` mode |
| `imu.calibrated`, `imu.g0`, `imu.fwd` | flag, g vector (3 numbers), variant 0–3 | not calibrated, `[0,0,1]`, `0` — set by calibration on the "Sensor" page |
| `wifi_ap.ssid` | a string, empty = `roundGauge-XXXX` from the MAC | empty |
| `wifi_ap.password` | 8–64 characters, WPA2; shorter than 8 — an open network | `roundgauge` |

## 6. Screens (layout)

The layout is a table of signals and a list of screens in one JSON
(`common/config/layout.h` has the full format). The built-in layout `layout_default.h` has 5 screens
with backgrounds and fonts from `fw/media`:

| # | Screen | Background | Needle |
|---|---|---|---|
| 1 | RPM — dial 0–8 (×1000), the number in thousands with 2 decimals | `bg_carbon` | `needle_classic` |
| 2 | COOLANT — ring | `bg_brushed` | line |
| 3 | AFR — dial 10–20, `lambda` below | `bg_hud` | `needle_neon` |
| 4 | SPEED — number | `bg_perf` | — |
| 5 | G-meter | `bg_hud` | — |

The layout is stored in NVS, edited from the editor and applied on the fly: `ui_task` notices the
change and rebuilds the screens.

A **signal** (up to 16) is described once: a name (Latin, up to 15 characters), a title, units,
`min`/`max`, decimals, up to 4 color zones. Screens and widgets refer to a signal by name and
inherit its parameters; a screen can set its own range and its own zones.

A **screen** (up to 8; no more than 4 different background files in the whole layout —
`RG_UI_MAX_BG_IMAGES`, checked when saving from the web, not checked when loading from NVS) is a
main widget plus up to 4 extra ones:

| Main | What it draws |
|---|---|
| `dial` | a dial: a scale with labels and zones, markers (up to 4), a line or picture needle; ticks and labels: `tick_major_len`/`tick_minor_len` (22/10), `tick_major_width`/`tick_minor_width` (4/2), `label_gap` (15: from the end of a major tick to the label center), `label_font` (`14`/`28`/`48` or `.fnt`) |
| `ring` | ring fill; the fill color follows the zone |
| `number` | nothing: only the extra widgets |
| `gmeter` | g-meter: a round grid, the g-force dot, a trail, maximum markers (§6.1) |

| Extra | What it does |
|---|---|
| `value` | the signal number; font, color, color by zone, decimals (`decimals`), divisor `div` (the value / `div` is shown, zones use the original value; 1 by default) |
| `text` | a static label |
| `image` | a picture from media |
| `indicator` | a dot or a picture, visible when above `>` / below `<` a threshold, 2 Hz blinking |
| `bar` | a linear bar, horizontal or vertical |
| `arc` | a mini arc |

The coordinates of extra widgets are pixels from the screen center, the widget is centered at that
point. They are drawn in list order, the bottom one first. The title, the value and the units are
ordinary extra widgets, the editor creates them by default.

**Images** are ready-made LVGL `.bin` files: RGB565 without transparency, ARGB8888 with
transparency; PNG/JPG are converted by the browser (`lvbin.js`). A background is up to 480×480; a
picture needle points right, the pivot is by default at the left center; the pivot point can be
set by the fields `needle_px`/`needle_py` (image pixels from the top-left corner, -1 — auto) — in
the editor it is set by clicking on the picture or with numbers. LVGL reads files through the POSIX
driver (`M:/name.bin` → `/media/name.bin`). No file — the screen is drawn with primitives.

**Image cache.** LVGL reads a `.bin` from flash into RAM on the first draw (a 480×480 background —
83 ms on the board). After building the screens, `ui_screens_rebuild` opens and closes every image
of the layout once (`preload_images`), the entry stays in the cache (`LV_CACHE_DEF_SIZE` 3 MB, the
key is the path and the source type). The log shows the time of each image, the total and a warning
if the images do not fit in the cache. On the board: 4 backgrounds and 2 needles — 1868 KB, 631 ms
at start and at every layout apply. The header cache is on (`LV_IMAGE_HEADER_CACHE_DEF_CNT` 32).
Screens are not unloaded: all are built at once and live until the next layout rebuild.

**Fonts.** The built-in Montserrat 14, 28, 48 are Latin only. Custom ones are LVGL bin fonts
(`lv_font_conv --format bin --bpp 4 --no-compress`; compressed fonts are off in the build), stored
in media as `*.fnt`, one file per size, loaded into RAM as a whole when a screen is built; no file —
font 28. Cyrillic and large digits only come with a custom font (range `0x400–0x4FF`). The size in
the file name (`seg7_120.fnt`) is needed by the editor preview (a number before the extension,
8–200, otherwise 28); on the board the size is set by the file itself.

The base set in `fw/media`: Segment7 (digits) 40/72/120; Orbitron 800 — 24/36 (Latin), 56/96
(digits); Michroma 24/36; Russo One 28/40 (Latin and Cyrillic), 64 (digits); Jura 600 24/36 (Latin
and Cyrillic); Black Ops One Slant (outlines slanted by 24°) 28/40 (Latin), 56/96 (digits). The
files `*_56`, `*_64`, `*_96`, `seg7_*` contain only digits and `. : - + % /`. The digits 0–9 have the same width
in all fonts (in the source fonts, except Segment7, it differs; it is equalized to the widest
digit, outlines centered in the cell), so a value does not jitter when digits change.
Backgrounds `bg_carbon`, `bg_hud`, `bg_brushed`, `bg_perf` (480×480 RGB565), needles
`needle_classic`, `needle_neon`, `needle_stripe` (199×44 ARGB8888, pivot 34,22). Licenses are in the README.

### 6.1. G-meter and accelerometer

**The sensor.** The board has a QMI8658 (I2C 0x6B on the shared bus, datasheet —
`hw/datasheets/QMI8658A.pdf`). `imu_task` sets it to ±4 g and 125 Hz, reads every 8 ms, smooths
(about 7 Hz), converts to the car axes and writes into the signal store (timeout 500 ms — if the
sensor goes silent, the values disappear, as when CAN is lost):

| Signal | Meaning |
|---|---|
| `g_lon` | longitudinal: > 0 forward (acceleration), < 0 backward (braking), g |
| `g_lat` | lateral: > 0 right (a right turn), g |
| `g_vert` | vertical relative to rest: > 0 up, g |
| `g_tot` | total horizontal, √(lon² + lat²), g |

The sensor on the board is always real: in the "demo" mode the generator does not touch these signals. These signals are ordinary:
any widget (number, bar, indicator) can show them on any screen.

**Orientation.** The board is mounted in any way. Calibration by gravity: while the car stands
level, the readings are averaged for about a second — the vector g0 points "up" at any board
tilt (a spread of readings above 0.16 g means the car moved and the calibration is rejected).
"Forward" is one of four horizontal directions (plus and minus of the two board axes with the
largest horizontal projection); it is chosen by number 1–4 or detected by acceleration (the next
horizontal acceleration above 0.12 g lasting more than 0.2 s). Right = forward × up. Linear
acceleration = reading − g0. The math is in `tasks/imu/imu_math.[ch]`. Limitation: body roll
and pitch change the projection of gravity and mix slightly into the readings.

**The `gmeter` screen.** A grid of rings with step `g_step` up to `g_range` (0.5 and 1.5 g by
default), a crosshair, a dot driven by the signals `signal` (forward-backward) and `signal2`
(left-right), a trail of up to 12 points (every 50 ms), maximum markers in four directions with
numbers (held for 8 s, then decaying at 0.25 g/s). By default (`felt`) the dot shows the force
the driver feels, as in racing: braking — up, a right turn — left; without `felt` — by the
acceleration of the car. The color `color` is the dot, `text_color` the grid. The g numbers are
ordinary extra widgets: four numbers by default (total, longitudinal, lateral, vertical). No
sensor data — the dot, trail and markers are hidden.

**Version 1** (`title`/`unit`/`show_value` right on the screen, without a signal table) is read
and turned into version 2 when parsed, in the same places.

The editor preview (`gauge-render.js`) repeats the geometry of `ui_screens.c` approximately;
custom fonts in it are drawn with a fallback font.

## 7. CAN

Reception is **passive**: the dashboard transmits nothing to the bus in any mode. The TWAI node is
created and re-created inside `can_task` (`esp_twai_onchip`), the bitrate and mode come from the
settings, and a change is picked up on the fly.

The **mapping table** (`common/config/can_map.h`, up to 16 entries, one per signal) works as in a
DBC:

```json
{ "signal": "rpm", "id": "0x201", "ext": false, "start": 7, "len": 16,
  "order": "motorola", "signed": false, "factor": 0.25, "offset": 0, "timeout": 2000 }
```

Value = `raw * factor + offset`. Bits are numbered exactly as in a DBC: for `intel` (@1)
`start` is the least significant bit, for `motorola` (@0) the most significant bit in DBC
numbering (7..0, 15..8, …). A field of 1–32 bits must fit in 8 bytes. `timeout` is the ms
without frames (100–60000, 2000 by default) after which the signal is considered invalid:
numbers show `--`, the needle goes to the start of the scale, indicators go out.

**The rusEFI preset** (`common/config/can_map_default.h`). While NVS has no saved table, the
built-in one applies: rusEFI by default broadcasts frames from base ID 0x200 at 500 kbit/s (the
board has the same default mode, `listen_only`), all fields are Intel (@1). 12 mappings: `rpm`,
`speed` (0x201), `tps` (0x202, TPS1, signed), `coolant`, `iat`, `map` (kPa), `fuel_level`
(0x203), `oil_press` (bar: raw * 0.0333 kPa / 100), `oil_temp`, `vbat` (V) (0x204), `lambda` and
`afr` (0x207; `afr` = lambda × 14.7 for gasoline, for another fuel the multiplier of the `afr`
mapping is changed, for example 9.76 for E85). The source is the rusEFI DBC. The preset signals
are in the signal table of the built-in layout (16 of 16: `rpm`, `coolant`, `speed`, `g_lon`,
`g_lat`, `g_vert`, `g_tot`, `iat`, `map`, `oil_press`, `oil_temp`, `vbat`, `fuel_level`, `lambda`,
`tps`, `afr`); the screens show `rpm`, `coolant`, `afr` (with `lambda`), `speed`, `g_*`, the rest
are available to widgets. An explicitly saved empty table stays empty. To bring the preset back:
the "rusEFI preset" button on the CAN page or `POST /api/can/map/reset`.

- **Filter in the ISR.** While the web does not read the sniffer, the ISR puts only the IDs from
  the table into the queue (two filter buffers, a pointer switch). Every `/api/can/frames`
  request lifts the filter for 3 s.
- **Sniffer**: the last frame of each ID, up to 64 IDs; the frame period (ms) and highlighting of
  changed bytes are computed by the browser.
- **Bus-off**: the ISR only sets a flag, `twai_node_recover()` is called from the task a second later.
- **Demo**: with `can.demo` the signals (except the accelerometer `g_*`) are driven by a generator
  (a triangle wave over the signal range), values from the bus are not written; turning it off
  resets the values.
- A signal without a mapping stays without data when "demo" is off.

## 8. Controls, Wi-Fi and web

### Buttons and gestures

| Action | Result |
|---|---|
| Button 1, short press | next screen; silences a running sound alert |
| Button 1, hold 2 s | bring up the Wi-Fi access point |
| Button 1, hold 10 s | clear the settings password. From the 7th second the screen shows "Hold to reset password", after the reset "Password reset"; release earlier — no reset. The access point comes up at the 2nd second meanwhile. The Wi-Fi password is not reset |
| Button 2, press | brightness cycle 100 → 60 → 30 → 10 → 100 % (`RG_BTN2_BRIGHTNESS_STEPS`; the next level is the first one below the current; saved in `display.brightness`; does not work with Type-C power, §3) |
| Swipe left / right | next / previous screen (only with `RG_HAS_TOUCH=1`) |

### Access point

Off; it goes out only with a restart. After a long press the card "Wi-Fi / Starting..." appears at
once; when the network and the site are ready, a card for 10 s (`RG_AP_CARD_MS`) with the network
name, the password and the address `http://192.168.4.1`, then a Wi-Fi icon with the number of
connected devices stays at the bottom of the screen. The SSID is `roundGauge-XXXX` from the MAC or
the one set in the settings, WPA2, up to 4 clients. The password is shown on the screen in full —
for the time of the card.

### Sound

The board buzzer is connected to pin EXIO8 of the TCA9554 expander (`RG_EXIO_BUZZER`): it is a
plain on/off over I2C, without PWM or tones. An active buzzer beeps with one tone, a passive one
may not sound at all on a constant level. `roundGauge_board_buzzer_set()` turns the pin on and off,
at initialization it is off.

`sound_task` (core 0, 10 ms period) decides whether the buzzer should sound, by the mode
`sound.mode` (§5) and the alert rules (`common/config/alerts.h`, up to 8):

```json
{ "signal": "coolant", "op": ">", "value": 105, "hyst": 3,
  "pattern": "beeps", "count": 3, "on": 120, "off": 120, "repeat": 5, "enabled": true }
```

- **Condition:** the signal value is above (`>`) or below (`<`) a threshold; the alert clears when
  the value is back beyond the threshold by `hyst`. A signal without data or a disabled rule — no alert.
- **Pattern** (`pattern`): `beeps` — a series of `count` beeps (1–9) of `on` ms with pauses of
  `off` ms (20–2000 each), the series repeats every `repeat` s (0 — once per trigger);
  `continuous` — non-stop while the condition holds.
- **Silencing:** any button press or swipe (`roundGauge_sound_user_input()`) silences the running
  alerts until the value leaves the condition; a new crossing of the threshold sounds again.
- **Modes:** `off` — silence (the test beep always works); `alerts` — alerts; `alerts_clicks` —
  also a short beep (`RG_SOUND_CLICK_MS`) on button presses and swipes.
- Several alerts at once are combined: the buzzer sounds if at least one of them asks for it.
- Built-in rules: coolant above 105 °C (3 beeps every 5 s), RPM above 7000 (2 beeps, once).

### Recording logs to SD

`logger_task` (core 0, 50 ms period) writes the values of **all signals of the current layout**
(up to 16) to microSD in the MegaLogViewer / TunerStudio format (MLG, version 1,
`tasks/logger/mlg.c`): 20 records per second, a `float32` per signal, the file `log_NNNN.mlg` in
the card root (an 8.3 name, the next number is looked up at mount). A signal without data is
written as zero, the format has no "empty" value. Units in the file are ASCII only (`°C` → `C`;
Cyrillic units → empty). Changing the layout during a recording closes the file and starts a new
one. The buffer goes to the card once a second (`RG_LOGGER_FLUSH_MS`), fsync every 2 s: on a power
cut up to a couple of seconds are lost. Recording stops when less than 4 MB is free or the card
does not respond.

**The card.** The microSD is used over SPI (`SPI2`): MOSI and SCK are the same GPIO1/GPIO2 as the
display 3-wire SPI (which is only needed at start and, while its CS on EXIO3 is high, does not react
to the card traffic), MISO is GPIO42. The card CS is on the expander (EXIO4), not a GPIO: while the
card is mounted, the pin is held low. FAT32 is required; a foreign card is not formatted.
`roundGauge_board_sd_mount()` and `_unmount()` are called only by `logger_task`. Mounting happens
3 s after power-on (by then the display has finished initialization) and then every 15 s in the "at
power-on" and "by condition" modes; in the "manual" mode the card is looked for on request ("Check
card" on the page and "Start recording"). A removed card is noticed within 2 s when idle, and by a
write error during recording.

**When to record** (`settings.log`, applied at once; the logic is in `logger_logic.c`):

| `log.mode` | Behavior |
|---|---|
| `manual` | only by the "Start recording" / "Stop" buttons on the "Logs" page |
| `boot` | once after power-on, as soon as the card is ready; a recording stopped by hand is not resumed until the next power-on |
| `condition` | while `signal` `op` (`>` or `<`) `value`, and for `stop_delay` s after the condition stops holding; no data for the signal — the condition does not hold |

A recording started by the button is not stopped by the condition. A recording stopped by the
button while the condition holds is not resumed until the condition goes away and comes back. After
a stop by an error (no space, a write error) the recording is likewise not resumed until the
condition goes away.

The board keeps no time anywhere: the "Logs" page sends the browser time when opened
(`POST /api/log/time`), which is needed for file dates and the timestamp in the log header. Until
the page is opened after power-on, the header timestamp is 0 and the file date is the year 1980
(not shown in the list).

### Web interface

The stack: Alpine.js, Bootstrap, FontAwesome (solid, woff2), light/dark theme, RU/EN, an optional
password on modifying requests.

| Page | What is on it |
|---|---|
| `/` | status (version, uptime, reset reason, access point clients, CAN, memory, space in media), navigation tiles, brightness |
| `/editor.html` | screens: a preview with dragging of widgets, the main widget, extra widgets, the signal table, JSON; fine settings in collapsible sections (the state is remembered in the browser) |
| `/media.html` | images and fonts on the board: a list with thumbnails, upload, delete |
| `/can.html` | bitrate, mode, "demo", bus state, the mapping table (the "rusEFI preset" button), a sniffer with frame periods |
| `/access.html` | the settings password and the Wi-Fi parameters of the access point (Wi-Fi changes only with a password set) |
| `/sound.html` | sound: mode (off / alerts / alerts + clicks), test beep, alert rules (signal, condition, threshold, hysteresis, pattern), alert state |
| `/logs.html` | logs: card and recording state, "Start recording" / "Stop" / "Check card", when to record (manual / at power-on / by condition), a file list with download and delete |
| `/imu.html` | sensor: a live g-force dot, calibration by gravity, the "forward" direction (by acceleration or by hand; the variants are labeled with the board axes and a live bar) |
| `/ota` | firmware and www image update; built into the firmware (`ota_page.h`) so it works with an empty www partition too |

### HTTP API

Reading (GET) is open, modifying (POST) is closed by the password if one is set (Basic Auth,
the login is ignored).

| Method | Path | Meaning |
|---|---|---|
| `GET` | `/api/status` | version, uptime, reset reason (`esp_reset_reason()`), the number of access point clients, memory (`heap`: internal RAM free/minimum/largest block, PSRAM); the result of the www update in `www_update` |
| `GET` | `/api/auth/status` | whether a password is on |
| `POST` | `/api/auth/verify` | password check |
| `POST` | `/api/auth/password` | set / change / clear (`""`) the password, 4–64 characters; while a password exists, the current one is required |
| `POST` | `/api/ota/update` | firmware into the free `ota_N` slot, then a restart |
| `POST` | `/api/www/update` | the whole www partition image (only differing blocks are written) |
| `GET` | `/api/display` | backlight brightness, % |
| `POST` | `/api/display` | change brightness (5–100), applied at once |
| `GET` | `/api/imu` | sensor: found or not, calibrated or not, the "forward" variant, calibration and detection state, acceleration in board axes (`raw`), the calibration vector `g0` and acceleration in car axes |
| `GET` | `/api/sound` | sound mode, masks of running and silenced alerts, buzzer state |
| `POST` | `/api/sound` | `{"mode": "off"}`, `"alerts"` or `"alerts_clicks"`; applied at once |
| `GET` | `/api/sound/rules` | alert rules |
| `POST` | `/api/sound/rules` | validate, save (`alerts`) and apply on the fly |
| `POST` | `/api/sound/rules/reset` | erase the saved rules and return the built-in ones |
| `POST` | `/api/sound/test` | test beep |
| `GET` | `/api/log` | state (`no_card` / `idle` / `recording`), the reason of the last stop (`error`: `mount`, `open`, `write`, `full`, `no_signals`), file, records, bytes, seconds, card size and free space in KB, and the start conditions (`mode`, `signal`, `op`, `value`, `stop_delay`) |
| `POST` | `/api/log` | change the start conditions (fields missing from the request stay as they were); for `condition` the signal must be in the layout |
| `POST` | `/api/log/start`, `/api/log/stop` | start / stop recording; executed by `logger_task` within 50 ms |
| `POST` | `/api/log/rescan` | try to mount the card now; an open request |
| `POST` | `/api/log/time` | `{"unix": seconds}`: set the board time; an open request |
| `GET` | `/api/log/files` | `{"files": [{"name", "size", "time"}]}`, newest first, up to 200; 409 if there is no card |
| `GET` | `/log/<name>` | download a `log_NNNN.mlg` file (one still being written — as of now) |
| `POST` | `/api/log/delete?name=log_0001.mlg` | delete a file; one being recorded — 409 |
| `GET` | `/api/wifi` | network name (set and default), the "network password is default" flag; the password itself is not returned |
| `POST` | `/api/wifi` | `{"ssid", "password"}`: name up to 32 bytes (empty — from the MAC), password 8–64; only with a settings password set (otherwise 403), applied after a restart |
| `POST` | `/api/imu/calibrate` | start calibration (the car stands still for about a second) |
| `POST` | `/api/imu/forward` | `{"fwd": 0..3}` choose "forward" by hand or `{"detect": true}` detect by acceleration |
| `GET` | `/api/layout` | the layout JSON |
| `POST` | `/api/layout` | validate, save to NVS and apply on the fly |
| `POST` | `/api/layout/reset` | return the built-in layout |
| `GET` | `/api/media` | the file list and the used space |
| `POST` | `/api/media?name=x.bin` | upload a file (up to 1 MB; through a temporary file) |
| `POST` | `/api/media/delete?name=x.bin` | delete a file |
| `GET` | `/media/<name>` | a file (for preview) |
| `GET` | `/api/can` | CAN settings and bus state: node, counters, errors |
| `POST` | `/api/can` | change bitrate / mode / demo, the node is re-created on the fly |
| `GET` | `/api/can/map` | the mapping table |
| `POST` | `/api/can/map` | validate, save (`can_map`) and apply on the fly |
| `POST` | `/api/can/map/reset` | erase the saved table and return the rusEFI preset |
| `GET` | `/api/can/frames` | the sniffer; the request turns on reception of foreign IDs for 3 s |

## 9. Dependencies

| Component | Version | What for |
|---|---|---|
| `lvgl/lvgl` | ^9 (9.6) | graphics |
| `espressif/esp_lvgl_port` | ^2.9.0 | the LVGL task, display |
| `espressif/esp_lcd_st7701` | ^2.0.2 | ST7701S RGB panel |
| `espressif/esp_io_expander_tca9554` | ^2.0.3 | display resets and CS |
| `espressif/esp_lcd_touch_cst816s` | ^1.1.2 | CST820 touch (the same address and touch registers) |
| `espressif/esp_lcd_panel_io_additions` | ^1.0.1 | 3-wire SPI with the CS on TCA9554 for the ST7701 init |
| `espressif/cjson` | ^1.7.19 | JSON of settings, layout and the CAN table (outside the IDF tree in v6) |

## 10. Build, versions, releases

- **Build options** (passed as `idf.py -D NAME=value build`, kept in the build cache, to bring
  back — `reconfigure` with the needed value):
  - `RG_HAS_TOUCH` (1 by default) — `0` for a display without a touch panel: the board does not
    initialize touch, `touch_task` does nothing, control stays on the buttons;
  - `RG_LOG_LEVEL` — `WARN` in a release build, otherwise `INFO`;
  - `RG_UI_SHOW_FPS` — the FPS and render time lines on the screen: `0` in a release build, otherwise `1`.
- **Constants** — `common/config/conf.h`: task sizes and priorities, layout limits, swipe
  thresholds, button timeouts and the access point card.
- **Version** — `0.<YY>.<MMDD>`; the release script writes it into `fw/version.txt`, and ESP-IDF
  takes the application version from there (it is returned in `/api/status`).
- **`tools/build/build.bat` / `build.sh`**: activate ESP-IDF themselves, generate
  `fw/www/.build_id`, build with `RG_LOG_LEVEL=WARN` and `RG_UI_SHOW_FPS=0`, immediately return
  the CMake cache to the debug build values (INFO, FPS on) and put a complete USB flashing set into
  `releases/roundGauge_v<version>_<time>/`: `roundGauge_firmware_v*.bin`, `roundGauge_www_v*.bin`,
  `roundGauge_media_v*.bin`, `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin`,
  `flash_args` and `README.txt`. Then `tools/build/package_release.py` turns the folder into a
  portable "on the desk" set: it adds `esptool.exe` (the official standalone v5.4.0, the version is
  pinned in the script, downloaded once into `tools/build/.cache`) with its license and the
  project `LICENSE.txt`, writes `flash_all.bat/.sh` (everything, 460800) and
  `update_firmware.bat/.sh` (application and web, `media` untouched) from `flash_args`, and builds
  `releases/roundGauge_v<version>_<time>.zip` next to it (one folder `roundGauge_v<version>`
  inside). The `releases/` folder is not in git.
- **`tools/webtest/mock_server.py`** serves `fw/www` and `/ota` from `ota_page.h`, emulates the §8
  endpoints, including CAN with fake frames.
- `.gitattributes` fixes CRLF for `*.bat` and LF for `*.sh`: otherwise `cmd.exe` or bash will not
  parse the script.

## 11. Open questions

1. **Performance of heavy screens:** the cost of a picture needle (rotation without an
   accelerator), screens with several widgets; an option is a snapshot of the static part (§3.2).
2. **Other button and gesture actions:** a long press of button 2, vertical swipes (for example,
   brightness).
3. **Updating media with an image through `/ota`:** only firmware and www for now.
4. **Signal slots:** a renamed signal stays in the store until a restart; with 16 different names
   in one run, new ones are not registered. The built-in layout takes all 16.
5. **CAN:** OBD-II requests (not needed while reception is passive), multiplexed signals, frame
   counters and checksums.
6. **Export and import** of the layout and the mapping table as a file.
7. **Task watchdog:** which tasks to attach it to.
8. **Accelerometer:** the QMI8658 gyroscope is not used (it could compensate body tilt).
9. **Built-in settings** (layout, CAN preset, alert rules) apply while NVS has no saved ones; after
   a firmware update on a board with saved ones you need "Reset to built-in" in the editor, "rusEFI
   preset" on the CAN page and "Defaults" on the "Sound" page.
