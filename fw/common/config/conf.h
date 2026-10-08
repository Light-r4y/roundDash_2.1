#pragma once

// Константы проекта. Номера GPIO сюда не попадают — они живут только в
// common/board/board.h.

// ------------------------------------------------------------------
// Задачи: стек, приоритет, ядро (docs/fw-design.md §2)
// ------------------------------------------------------------------
// Ядро 1 целиком под отрисовку, всё остальное — на ядре 0 вместе с Wi-Fi.
// Wi-Fi включается только в режиме настройки, поэтому в обычной езде CAN и
// кнопки ядро 0 ни с кем не делят.

// Задача esp_lvgl_port: крутит lv_timer_handler и рендер.
#define RG_LVGL_TASK_STACK 8192
#define RG_LVGL_TASK_PRIORITY 4
#define RG_LVGL_TASK_CORE 1

// ui_task: переносит значения сигналов в виджеты под lvgl_port_lock().
#define RG_UI_TASK_STACK 8192
#define RG_UI_TASK_PRIORITY 3
#define RG_UI_TASK_CORE 1
#define RG_UI_TASK_PERIOD_MS 16

#define RG_CAN_TASK_STACK 4096
#define RG_CAN_TASK_PRIORITY 8
#define RG_CAN_TASK_CORE 0

#define RG_BUTTONS_TASK_STACK 2560
#define RG_BUTTONS_TASK_PRIORITY 5
#define RG_BUTTONS_TASK_CORE 0
#define RG_BUTTONS_TICK_MS 10

// touch_task: свайпы по экрану. Выше CAN-ISR не лезет, но выше httpd, чтобы жест не ждал
// веб-сервер. Тач читается по прерыванию контроллера, пока палец на экране - ещё и
// сам каждые RG_TOUCH_POLL_MS (прерывание при подъёме пальца может не прийти).
#define RG_TOUCH_TASK_STACK 3072
#define RG_TOUCH_TASK_PRIORITY 6
#define RG_TOUCH_TASK_CORE 0
#define RG_TOUCH_POLL_MS 20
// Свайп: горизонтальное смещение от точки касания не меньше MIN_PX, заметно больше
// вертикального, за не более MAX_MS. Срабатывает сразу, не дожидаясь подъёма пальца.
#define RG_TOUCH_SWIPE_MIN_PX 60
#define RG_TOUCH_SWIPE_MAX_MS 800

// imu_task: акселерометр QMI8658, 125 Гц (tasks/imu). Сигналы g_* живут столько мс без новых
// отсчётов, потом пропадают (датчик замолчал).
#define RG_IMU_TASK_STACK 3072
#define RG_IMU_TASK_PRIORITY 4
#define RG_IMU_TASK_CORE 0
#define RG_IMU_SIGNAL_TIMEOUT_MS 500

// sound_task: зуммер (EXIO8 расширителя TCA9554), тревоги по правилам и клики кнопок (tasks/sound).
#define RG_SOUND_TASK_STACK 3072
#define RG_SOUND_TASK_PRIORITY 3
#define RG_SOUND_TASK_CORE 0
#define RG_SOUND_TICK_MS 10
#define RG_SOUND_CLICK_MS 25  // писк при нажатии кнопки, режим "тревоги + клики"
#define RG_SOUND_TEST_MS 300  // пробный писк со страницы "Звук"

// Правила тревог: JSON одним blob'ом (common/config/alerts.h).
#define RG_ALERTS_NVS_KEY "alerts"
#define RG_ALERTS_JSON_MAX 2048

// webcfg_task: подъём точки доступа и HTTP-сервера по запросу.
#define RG_WEBCFG_TASK_STACK 4096
#define RG_WEBCFG_TASK_PRIORITY 2
#define RG_WEBCFG_TASK_CORE 0
#define RG_WEBCFG_TICK_MS 100

// Задачу httpd создаёт esp_http_server, сюда — только её параметры.
#define RG_HTTPD_STACK 8192
#define RG_HTTPD_PRIORITY 5
#define RG_HTTPD_CORE 0

// ------------------------------------------------------------------
// Экран
// ------------------------------------------------------------------
// Яркость подсветки по умолчанию, %; меняется в вебе и хранится в настройках.
#define RG_LCD_BL_DEFAULT_PCT 100
// Ниже экран практически не видно, поэтому веб и NVS ниже не пускают.
#define RG_LCD_BL_MIN_PCT 5
// Кнопка 2: уровни яркости по кругу, % (от большего к меньшему; после последнего - снова первый).
#define RG_BTN2_BRIGHTNESS_STEPS { 100, 60, 30, 10 }
// Сколько мс висит короткое сообщение на экране (roundGauge_ui_toast()).
#define RG_TOAST_MS 3000

// Экран приборки (tasks/ui, common/config/layout.h)
#define RG_UI_MAX_SCREENS 8
// Сколько мс после запуска точки доступа на экране висит карточка с именем сети, паролем
// и адресом; потом остаётся только значок Wi-Fi с числом подключённых.
#define RG_AP_CARD_MS 10000
// Экран с тач-панелью (1) или без (0). Задаётся при сборке: idf.py -D RG_HAS_TOUCH=0 build
// (fw/CMakeLists.txt); здесь только значение на случай, если define не пришёл.
#ifndef RG_HAS_TOUCH
#define RG_HAS_TOUCH 1
#endif
// Размер шкалы/кольца на круглом экране, px.
#define RG_UI_DIAL_SIZE 440
// Счётчик кадров и время отрисовки сверху экрана. Задаётся при сборке: idf.py -D RG_UI_SHOW_FPS=0 build
// (по умолчанию 1; релизная сборка tools/build/build.* передаёт 0).
#ifndef RG_UI_SHOW_FPS
#define RG_UI_SHOW_FPS 1
#endif

// Раскладка экранов: JSON одним blob'ом в том же пространстве NVS, что настройки.
#define RG_LAYOUT_NVS_KEY "layout"
#define RG_LAYOUT_JSON_MAX 16384
// Разных файлов фона на все экраны: кэш картинок (3 МБ) держит около 6 фонов 480x480,
// остальное - запас. Экраны с одним и тем же файлом считаются за один фон.
#define RG_UI_MAX_BG_IMAGES 4

// ------------------------------------------------------------------
// Кнопки
// ------------------------------------------------------------------
// Сколько тактов подряд кнопка должна быть нажата, чтобы это считалось нажатием.
#define RG_BTN_DEBOUNCE_TICKS 3
#define RG_BTN_COUNT 2
// Удержание кнопки 1 поднимает точку доступа. Гаснет она только с перезагрузкой.
#define RG_BTN_AP_HOLD_MS 2000

// ------------------------------------------------------------------
// CAN
// ------------------------------------------------------------------
// Кадры из ISR идут в can_task через очередь. Длину (64) ещё надо сверить с реальной
// загрузкой шины: счётчик потерянных кадров виден на странице CAN.
#define RG_CAN_RX_QUEUE_LEN 64

// Значения по умолчанию, пока в NVS пусто: новая приборка ничего не передаёт в
// шину, пока её не настроят.
// Таблица привязки сигналов к кадрам: JSON одним blob'ом (common/config/can_map.h).
#define RG_CAN_MAP_NVS_KEY "can_map"
#define RG_CAN_MAP_JSON_MAX 4096

// Сниффер для веба: сколько разных ID помнит плата, и сколько мс после последнего
// запроса страницы кадры чужих ID ещё принимаются (без сниффера ISR отбрасывает всё,
// чего нет в таблице привязок).
#define RG_CAN_SNIFFER_MAX 64
#define RG_CAN_SNIFFER_ACTIVE_MS 3000

// Через сколько мс после bus-off узел восстанавливается.
#define RG_CAN_BUSOFF_RECOVERY_MS 1000

#define RG_CAN_BITRATE_DEFAULT 500000
#define RG_CAN_LISTEN_ONLY_DEFAULT true

// ------------------------------------------------------------------
// Wi-Fi: точка доступа поднимается только кнопкой
// ------------------------------------------------------------------
// SSID по умолчанию — префикс + хвост MAC (roundGauge-XXXX). SSID и пароль
// меняются через веб и хранятся в NVS.
#define RG_WIFI_AP_SSID_PREFIX "roundGauge-"
#define RG_WIFI_AP_PASS_DEFAULT "roundgauge"
#define RG_WIFI_AP_SSID_MAX_LEN 32
// WPA2-PSK требует пароль длиной 8-64 символа.
#define RG_WIFI_AP_PASSWORD_MIN_LEN 8
#define RG_WIFI_AP_PASSWORD_MAX_LEN 64

// ------------------------------------------------------------------
// Разделы SPIFFS (partitions.csv)
// ------------------------------------------------------------------
#define RG_WWW_PARTITION "www"
#define RG_WWW_BASE_PATH "/www"
#define RG_MEDIA_PARTITION "media"
#define RG_MEDIA_BASE_PATH "/media"
// Предел одного загружаемого файла: фон 480x480 с прозрачностью (ARGB8888) - 922 КБ.
#define RG_MEDIA_MAX_FILE 1000000

// ------------------------------------------------------------------
// Настройки в NVS (common/config/settings.c)
// ------------------------------------------------------------------
// JSON целиком одним blob'ом. Добавление полей старые настройки не сбрасывает:
// чего нет в JSON, то берётся из значений по умолчанию.
#define RG_SETTINGS_NVS_NAMESPACE "rg_cfg"
#define RG_SETTINGS_NVS_KEY "cfg"
#define RG_SETTINGS_VERSION 1

// ------------------------------------------------------------------
// Пароль на OTA и изменяющие запросы API (common/config/auth.c)
// ------------------------------------------------------------------
#define RG_AUTH_NVS_KEY "auth_hash"          // SHA-256(salt + пароль), пусто/отсутствует = защита выключена
#define RG_AUTH_SALT "roundGauge-auth-v1-"   // фиксированная "соль", вкомпилирована в прошивку
#define RG_AUTH_PASSWORD_MIN_LEN 4
#define RG_AUTH_PASSWORD_MAX_LEN 64
// Забытый пароль: удержание кнопки 1 столько мс снимает его (точка доступа поднимется
// раньше, на RG_BTN_AP_HOLD_MS). При включении кнопку держать нельзя - это GPIO0 (BOOT),
// с ним на сбросе ROM-загрузчик уходит в режим прошивки.
#define RG_AUTH_RESET_HOLD_MS 10000
// С этого момента удержания кнопки 1 на экране предупреждение о предстоящем сбросе.
#define RG_AUTH_RESET_WARN_MS 7000
#define RG_AUTH_REALM "roundGauge"           // realm для Basic Auth (что видит браузер в диалоге)
