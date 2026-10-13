#pragma once
#include <stdbool.h>
#include <stdint.h>

// Решение "писать или нет".
//
// Режимы (числа совпадают с roundGauge_log_mode_t в settings.h):
//   0 manual    - только по кнопкам "Начать" / "Остановить" на странице "Логи";
//   1 boot      - один раз после включения, как только готова карта; остановленную вручную
//                 запись до следующего включения не возобновляет;
//   2 condition - пишет, пока сигнал выше (или ниже) порога, и ещё stop_delay секунд после того,
//                 как условие перестало выполняться. Нет данных по сигналу = условие не выполнено.
//                 Остановленную вручную запись возобновляет только новый заход условия.
// Запущенная вручную запись условием не останавливается.

#define RG_TRIG_MANUAL 0
#define RG_TRIG_BOOT 1
#define RG_TRIG_CONDITION 2

typedef struct {
    int mode;
    bool less;           // false: сигнал > value, true: сигнал < value
    float value;
    uint16_t stop_delay; // с
} rg_trig_cfg_t;

typedef struct {
    bool rec;           // запись идёт
    bool manual;        // запущена вручную: условие её не останавливает
    bool boot_done;     // режим boot уже отработал
    bool latched;       // остановлена, пока условие не пропадёт
    bool false_pending; // условие пропало, ждём stop_delay
    uint32_t false_since_ms;
} rg_trig_t;

typedef enum {
    RG_TRIG_NONE = 0,
    RG_TRIG_START,
    RG_TRIG_STOP,
} rg_trig_act_t;

// Один шаг. card_ok - карта готова; sig_valid/sig - значение сигнала условия (sig_valid=false: данных нет).
// req_start/req_stop - нажатия на странице "Логи". Возвращает действие; состояние rec меняется внутри.
rg_trig_act_t rg_trig_step(rg_trig_t *t, const rg_trig_cfg_t *cfg, bool card_ok, bool sig_valid, float sig,
                           bool req_start, bool req_stop, uint32_t now_ms);

// Запись остановлена снаружи (ошибка карты, нет места, смена раскладки без перезапуска):
// автоматически она не возобновится, пока условие не пропадёт.
void rg_trig_forced_stop(rg_trig_t *t);
