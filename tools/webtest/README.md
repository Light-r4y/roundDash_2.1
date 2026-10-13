# Manual testing of the web interface (no firmware or hardware)

[Русский](README_ru.md)

A mock server for checking the pages in `fw/www` in an ordinary browser on a computer,
without an ESP32 and without building the firmware.

## Running

```
python tools/webtest/mock_server.py
```

(or `python3`; the default port is 8088, another port is the first argument:
`python mock_server.py 8090`; `--password PASSWORD` turns on password protection)

Then open in a browser:

- `http://localhost:8088/` — home: status, brightness
- `http://localhost:8088/editor.html` — screen editor
- `http://localhost:8088/media.html` — images and fonts
- `http://localhost:8088/can.html` — CAN settings, mapping, sniffer
- `http://localhost:8088/logs.html` — logging to SD: recording, start conditions, file list
- `http://localhost:8088/imu.html` — sensor: calibration, "forward" direction, live dot
- `http://localhost:8088/access.html` — settings password and the access point Wi-Fi
- `http://localhost:8088/sound.html` — sound: mode, alert rules, test beep
- `http://localhost:8088/ota` — firmware and web interface update

## What it does

- Serves the static files straight from `fw/www` — the same set that goes into the
  www partition image. Edits show up after a page refresh.
- Serves `/ota` straight from `fw/tasks/webcfg/ota_page.h` — the real page from the
  firmware is what gets tested.
- Takes the built-in layout from `fw/common/config/layout_default.h` — the same one
  that is compiled into the firmware.
- Replaces the `/api/*` endpoints from `docs/fw-design.md` §8:
  - `/api/status`, `/api/auth/status`, `/api/auth/verify`, `/api/auth/password`;
  - `/api/display` (brightness);
  - `/api/wifi` — network name and password; as on the board, they change only when a settings password is set;
  - `/api/layout`, `/api/layout/reset` — the layout is validated as in the firmware
    (JSON, at least one valid screen, up to 16 KB, no more than 4 different backgrounds);
  - `/api/media`, `/api/media/delete`, `/media/<file>` — files are kept in memory;
  - `/api/can`, `/api/can/map` (the rusEFI preset from `can_map_default.h` by default), `/api/can/map/reset`,
    `/api/can/frames` — settings and mapping are kept in memory, `frames` returns fake
    frames with changing data (IDs `0x201`, `0x3B3`, `0x420`, `0x7E8` and one 29-bit).
    Like the board, the first request after a pause returns an empty list: receiving
    foreign IDs is turned on by the request itself;
  - `/api/log`, `/api/log/start|stop|rescan|time|files|delete`, `/log/<file>` — an in-memory "card",
    recording is simulated; a downloaded file is a real MLG with four fields and opens in MegaLogViewer;
  - `/api/sound`, `/api/sound/rules` (the rules from `alerts_default.h` by default), `/api/sound/rules/reset`,
    `/api/sound/test` — mode and rules are kept in memory, the alert of the first rule is simulated (5 s out of every 10);
  - `/api/imu`, `/api/imu/calibrate`, `/api/imu/forward` — smooth movement of the dot; calibration and
    "forward" detection take 1–3 s, as on the board, and each "forward" variant rotates the axes by 90 degrees;
  - `/api/ota/update`, `/api/www/update` — the upload is only simulated.

## Limitations

- State is kept only in the process memory — restarting the server resets it
  (layout, CAN table, images, password, brightness).
- Image uploads to `/ota` are only simulated, nothing is written. Failure scenarios
  are switched on by a parameter in the endpoint address: `?fail=1` — write error,
  `?hang=1` — dropped connection, `?stall=1` — no response, `?same=1` — the same
  image uploaded.
- The mock does not draw the screen or drive signals: the only live view is the
  preview in the editor. Validation of CAN table entries is simpler than in
  `can_map.c` (the client-side rules of the `can.html` page are stricter and match
  the firmware).
- In a browser panel built into a development environment, Alpine may not update
  `x-show` until the page is painted (it is applied through
  `requestAnimationFrame`); an ordinary browser does not have this.
