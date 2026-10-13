# hw — module documentation

[Русский](README_ru.md)

The Waveshare ESP32-S3-Touch-LCD-2.1 that the firmware (`fw/`) runs on.
All files come from the [Waveshare wiki](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1),
downloaded on 2026-10-02.

## board/ — the board

| File | What | Source |
|---|---|---|
| `ESP32-S3-Touch-LCD-2.1_schematic_diagram.pdf` | board schematic — the pin map in `fw/common/board/board.h` was made from it | [link](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1_schematic_diagram.pdf) |
| `ESP32-S3-Touch-LCD-2.1_structure.zip` | mechanics: a dimensioned drawing (PDF) and a 3D assembly model (STEP, 24.6 MB unpacked) — for an enclosure. **Not in git**, download it from the link | [link](https://files.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1/ESP32-S3-Touch-LCD-2.1_structure.zip) |

## datasheets/ — chips on the board

| File | Chip | Role on the board |
|---|---|---|
| `ST7701S_SPEC_V1.4.pdf` | ST7701S | 480×480 display controller, RGB + 3-wire SPI for initialization |
| `tca9554.pdf` | TCA9554 | I2C expander at 0x20: display and touch resets, display and SD chip selects, IMU/RTC interrupts, buzzer |
| `QMI8658A.pdf` | QMI8658 | accelerometer + gyroscope, I2C 0x6B |
| `PCF85063A.pdf` | PCF85063A | real-time clock, I2C 0x51 |
| `CH343DS1-en.pdf` | CH343P | USB-UART on Type-C (UART0), auto-download |
| `FSUSB42UMX_Datasheet.pdf` | FSUSB42 | switch that routes UART0 between the CH343 and the J9 connector — the reason button 2 does not work while powered over Type-C |

The datasheets come from `https://files.waveshare.com/wiki/common/<file name>`.

Waveshare has no datasheet for the CST820 touch controller.
