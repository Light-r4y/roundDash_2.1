# roundGauge — CAN-приборка на ESP32-S3

**Русский** | [English](README_en.md)

[![Platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](https://www.espressif.com/en/products/socs/esp32-s3)
[![Framework](https://img.shields.io/badge/framework-ESP--IDF%20v6.0-red)](https://github.com/espressif/esp-idf)
[![GUI](https://img.shields.io/badge/GUI-LVGL%209-orange)](https://lvgl.io)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Прошивка приборки для автомобиля на круглом сенсорном экране
[Waveshare ESP32-S3-Touch-LCD-2.1](https://www.waveshare.com/wiki/ESP32-S3-Touch-LCD-2.1)
(480×480): данные берутся из шины CAN, что и как показывать, настраивается
через веб-страницу по Wi-Fi, две физические кнопки — на разъёме платы.

> 🚧 **Состояние: каркас.** Экран и тач работают — после прошивки на экране
> тестовая шкала. CAN, Wi-Fi и веб-настройка — заглушки с `TODO`. Принятые
> решения и открытые вопросы — в [docs/fw-design.md](docs/fw-design.md).

## ✨ Возможности

| | Что | Состояние |
|---|---|---|
| 🖥️ | Экран ST7701S 480×480, LVGL 9, два кадровых буфера без разрывов картинки | ✅ работает |
| 👆 | Тач CST820 | ✅ работает |
| 🚌 | Приём CAN (TWAI), разбор сигналов по таблице из настроек | ⏳ заглушка |
| 🎛️ | Режим CAN: только прослушивание (по умолчанию) или нормальный — для запросов OBD-PID | ⏳ заглушка |
| 📶 | Точка доступа Wi-Fi по долгому нажатию кнопки 1 | ⏳ заглушка |
| 🌐 | Веб-настройка: светлая/тёмная тема, RU/EN, пароль на изменения | ⏳ только шапка |
| 🔄 | Обновление прошивки и веб-интерфейса через `/ota`, откат неудачного обновления | ⏳ страница есть, сервера нет |
| 🖼️ | Фоны и стрелки приборки из раздела `media`, дозагрузка своих через веб | ⏳ заглушка |

## 🔧 Железо

- **Плата** Waveshare ESP32-S3-Touch-LCD-2.1: ESP32-S3R8, 16 МБ флеш, 8 МБ PSRAM.
  Схема и даташиты — в [hw/](hw/README.md).
- **CAN-трансивер** 3.3 В (SN65HVD230 или TJA1051T/3) — на плате его нет.
- **Питание** в машине — DC-DC 12→5 В с защитой от бросков напряжения.

Всё подключается к разъёму J9 «12PIN Multi-function Interface»:

| Пин J9 | Сигнал | Назначение |
|---|---|---|
| 2 | 5V | питание от DC-DC |
| 1, 5, 11 | GND | |
| 6 | 3V3 | питание трансивера |
| 3 | GPIO19 | CAN RX |
| 4 | GPIO20 | CAN TX |
| 12 | GPIO0 | кнопка 1 (параллельно BOOT) |
| 10 | GPIO44 | кнопка 2 |

> ⚠️ В машине питайте плату через пин 2 J9, а не через Type-C: пока на Type-C
> есть питание, переключатель на плате отдаёт GPIO44 USB-UART, и кнопка 2 не
> работает. Подробности — [docs/fw-design.md §3](docs/fw-design.md).

## 📁 Структура

```
├── fw/                 прошивка (проект ESP-IDF)
│   ├── main/           инициализация и запуск задач
│   ├── tasks/          по компоненту на задачу: can, ui, buttons, webcfg
│   ├── common/         board (пины, экран, тач), config (настройки, пароль)
│   ├── www/            веб-интерфейс → раздел www
│   └── media/          картинки приборки → раздел media
├── docs/fw-design.md   устройство прошивки, решения, открытые вопросы
├── hw/                 схема платы, даташиты, механика
└── tools/
    ├── build/          релизная сборка
    └── webtest/        мок-сервер для веб-интерфейса
```

## 📋 Требования

- [ESP-IDF v6.0](https://docs.espressif.com/projects/esp-idf/en/v6.0/esp32s3/get-started/)
- Плата Waveshare ESP32-S3-Touch-LCD-2.1, кабель USB Type-C

Зависимости (LVGL, драйверы экрана и тача) скачиваются менеджером компонентов
при первой сборке.

## 🚀 Быстрый старт

```bash
cd fw
idf.py build
idf.py -p COM5 flash monitor
```

Порт — тот, под которым определился USB-UART CH343 на Type-C платы; режим
прошивки включается сам. Цель (`esp32s3`), флеш 16 МБ, PSRAM и таблица разделов
уже заданы в `fw/sdkconfig.defaults`.

После прошивки на экране — круглая шкала, стрелку гоняет анимация, в центре
версия прошивки; точка под пальцем показывает, что тач работает.

> 💡 **Windows:** если ESP-IDF ставился через EIM, а системный Python новее
> 3.11, `export.ps1` не найдёт своё окружение. Перед ним поставьте Python из
> состава IDF первым в `PATH`:
> ```powershell
> $env:PATH = "$env:USERPROFILE\.espressif\tools\idf-python\3.11.2;$env:PATH"; . $env:USERPROFILE\esp\v6.0\esp-idf\export.ps1
> ```

## 📦 Релизная сборка

```bash
tools/build/build.sh        # Linux/macOS
tools\build\build.bat       # Windows
```

Скрипт сам находит ESP-IDF, если окружение не активировано, ставит версию
`0.<ГГ>.<ММДД>` в `fw/version.txt`, собирает с уровнем логов WARN и складывает
в `releases/roundGauge_v<версия>_<время>/` полный комплект: прошивку, образы
`www` и `media`, загрузчик, таблицу разделов, `ota_data_initial.bin` и
`flash_args`. Новая плата прошивается из этой папки одной командой:

```bash
python -m esptool --chip esp32s3 -b 460800 --before default-reset --after hard-reset write-flash "@flash_args"
```

## 🌐 Веб-интерфейс без железа

```bash
python tools/webtest/mock_server.py
```

Открыть `http://localhost:8088/`. Сервер раздаёт `fw/www` как есть и
имитирует API платы — подробности в [tools/webtest/README.md](tools/webtest/README.md).

## 🧩 Сторонний код

| Что | Где | Лицензия |
|---|---|---|
| Init-последовательность и тайминги ST7701 от Waveshare из [ESP32_Display_Panel](https://github.com/esp-arduino-libs/ESP32_Display_Panel) | `fw/common/board/st7701_init.h` | Apache-2.0 |
| [Alpine.js](https://alpinejs.dev) 3.14.1 | `fw/www/alpine.min.js` | MIT |
| [Bootstrap](https://getbootstrap.com) 5.3.0 | `fw/www/bootstrap.min.css` | MIT |
| [Font Awesome Free](https://fontawesome.com) 6.4.0 | `fw/www/fontawesome/` | иконки CC BY 4.0, шрифт SIL OFL 1.1, код MIT |

LVGL (MIT) и компоненты Espressif (Apache-2.0) скачиваются при сборке и в
репозитории не лежат.

## 📄 Лицензия

[MIT](LICENSE)
