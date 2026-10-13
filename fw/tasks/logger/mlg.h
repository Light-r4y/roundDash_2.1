#pragma once
#include <stddef.h>
#include <stdint.h>

// Бинарный лог TunerStudio / MegaLogViewer (MLVLG, формат версии 1, EFI Analytics: MLG_Binary_LogFormat_1.0.pdf).
// Все числа big-endian.
//
// Файл: заголовок (22 байта) + описания полей (по 55 байт) + необязательная информационная строка,
// затем записи подряд. Запись: тип 0, счётчик (1 байт, по кругу), время (2 байта, шаг 10 мкс, по кругу
// за 655,36 мс), значения полей (float32 каждое), контрольная сумма (сумма байт значений, 1 байт).

#define MLG_NAME_MAX 33   // имя поля: 34 байта с завершающим нулём
#define MLG_UNITS_MAX 9   // единицы: 10 байт с завершающим нулём
#define MLG_HEADER_BYTES 22
#define MLG_FIELD_BYTES 55
#define MLG_RECORD_OVERHEAD 5 // тип, счётчик, время (2), сумма

typedef struct {
    char name[MLG_NAME_MAX + 1];
    char units[MLG_UNITS_MAX + 1];
    int8_t digits; // знаков после точки при показе
} mlg_field_t;

// Размер заголовка с описаниями полей и информационной строкой (info может быть NULL).
size_t mlg_header_size(int nfields, const char *info);

// Записать заголовок в buf (cap >= mlg_header_size). unix_ts - время начала (0 - неизвестно).
// Возвращает размер заголовка или 0, если buf мал или полей нет / слишком много.
size_t mlg_build_header(uint8_t *buf, size_t cap, const mlg_field_t *fields, int nfields, const char *info,
                        uint32_t unix_ts);

// Размер одной записи.
static inline size_t mlg_record_size(int nfields)
{
    return (size_t)nfields * 4 + MLG_RECORD_OVERHEAD;
}

// Записать запись в buf (cap >= mlg_record_size). counter - номер записи (берётся младший байт),
// time_us - время от начала записи в микросекундах. Возвращает размер записи.
size_t mlg_build_record(uint8_t *buf, size_t cap, uint8_t counter, uint64_t time_us, const float *values, int nfields);
