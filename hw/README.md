# hw — документация модуля

Waveshare ESP32-S3-Touch-LCD-2.1, на котором работает прошивка (`fw/`).
Источник всех файлов — [wiki Waveshare](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1),
скачаны 2026-10-02.

## board/ — плата

| Файл | Что | Источник |
|---|---|---|
| `ESP32-S3-Touch-LCD-2.1_schematic_diagram.pdf` | схема платы — по ней составлена карта пинов в `fw/common/board/board.h` | [ссылка](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1_schematic_diagram.pdf) |
| `ESP32-S3-Touch-LCD-2.1_structure.zip` | механика: чертёж с размерами (PDF) и 3D-модель сборки (STEP, 24.6 МБ распакованной) — для корпуса. **Не в git**, скачивается по ссылке | [ссылка](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1_structure.zip) |

## datasheets/ — микросхемы на плате

| Файл | Микросхема | Роль на плате |
|---|---|---|
| `ST7701S_SPEC_V1.4.pdf` | ST7701S | контроллер экрана 480×480, RGB + 3-wire SPI для инициализации |
| `tca9554.pdf` | TCA9554 | расширитель I2C 0x20: сбросы экрана и тача, CS экрана и SD, прерывания IMU/RTC, зуммер |
| `QMI8658A.pdf` | QMI8658 | акселерометр + гироскоп, I2C 0x6B |
| `PCF85063A.pdf` | PCF85063A | часы реального времени, I2C 0x51 |
| `CH343DS1-en.pdf` | CH343P | USB-UART на Type-C (UART0), автозагрузка |
| `FSUSB42UMX_Datasheet.pdf` | FSUSB42 | переключатель UART0 между CH343 и разъёмом J9 — из-за него кнопка 2 не работает при питании от Type-C |

Источник даташитов — `https://files.waveshare.com/wiki/common/<имя файла>`.

Даташита на тач CST820 у Waveshare нет.
