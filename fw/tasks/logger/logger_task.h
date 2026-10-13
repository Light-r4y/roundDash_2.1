#pragma once
#include <stdbool.h>
#include <stdint.h>

// Запись лога на microSD в формате MegaLogViewer (mlg.h): все сигналы текущей раскладки, 20 Гц, файл
// log_NNNN.mlg в корне карты. Когда писать - решают настройки log (settings.h, logger_logic.h) и кнопки
// на странице "Логи". Карта FAT32, монтируется самой задачей (board.h: roundGauge_board_sd_mount).

typedef enum {
    RG_LOG_NO_CARD = 0, // карты нет или не смонтировалась
    RG_LOG_IDLE,        // карта готова, запись не идёт
    RG_LOG_RECORDING,
} roundGauge_log_state_t;

typedef enum {
    RG_LOG_ERR_NONE = 0,
    RG_LOG_ERR_MOUNT,     // карта не смонтировалась (нет карты, не FAT32, не отвечает)
    RG_LOG_ERR_OPEN,      // не удалось создать файл
    RG_LOG_ERR_WRITE,     // ошибка записи (карту вынули?)
    RG_LOG_ERR_FULL,      // кончилось место
    RG_LOG_ERR_NO_SIGNALS // в раскладке нет сигналов
} roundGauge_log_error_t;

typedef struct {
    roundGauge_log_state_t state;
    roundGauge_log_error_t error; // причина последней остановки или неудачи; сбрасывается новым пуском
    char file[16];                // текущий (или последний) файл, "" - не было
    uint32_t records;
    uint32_t bytes;
    uint32_t seconds;             // длительность записи
    uint64_t total_kb, free_kb;   // 0, если карты нет
} roundGauge_log_status_t;

void roundGauge_logger_task_start(void);
void roundGauge_logger_get_status(roundGauge_log_status_t *out);

// Запросы со страницы "Логи": выполняет logger_task в течение одного такта (50 мс).
void roundGauge_logger_request_start(void);
void roundGauge_logger_request_stop(void);
// Попробовать смонтировать карту сейчас (в режиме "вручную" она сама не ищется).
void roundGauge_logger_request_rescan(void);

// Корень карты (RG_LOG_MOUNT_PATH), если она смонтирована, иначе NULL. Для списка и скачивания файлов.
const char *roundGauge_logger_root(void);

// Имя файла лога вида log_0001.mlg (в любом регистре).
bool roundGauge_logger_valid_name(const char *name);

// Карта пропала при обращении к ней (не открылась папка): размонтировать на ближайшем такте.
void roundGauge_logger_card_lost(void);
