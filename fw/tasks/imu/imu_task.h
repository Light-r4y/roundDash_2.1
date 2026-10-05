#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Акселерометр QMI8658 (I2C 0x6B на общей шине платы): читает ускорение, приводит его к
// осям машины (imu_math.h) и пишет в хранилище сигналов:
//   g_lon  - продольное, > 0 вперёд (разгон), < 0 назад (торможение), g
//   g_lat  - поперечное, > 0 вправо (правый поворот), g
//   g_vert - вертикальное относительно покоя, > 0 вверх, g
//   g_tot  - полное горизонтальное, sqrt(lon^2 + lat^2), g
// Сигналы живут RG_IMU_SIGNAL_TIMEOUT_MS: если датчик замолчал, значения пропадают.
//
// Ориентация: "вверх" запоминается калибровкой по силе тяжести (машина стоит ровно),
// "вперёд" - одно из четырёх горизонтальных направлений: номером или разгоном.
// Калибровка и направление хранятся в настройках (раздел imu).
//
// В режиме "демо" (can.demo) значения сигналов гонит генератор, датчик их не пишет.

#define RG_SIG_G_LON "g_lon"
#define RG_SIG_G_LAT "g_lat"
#define RG_SIG_G_VERT "g_vert"
#define RG_SIG_G_TOT "g_tot"

typedef enum {
    RG_IMU_CAL_IDLE = 0,
    RG_IMU_CAL_RUNNING,
    RG_IMU_CAL_DONE,
    RG_IMU_CAL_MOVED,  // машина двигалась во время калибровки
} roundGauge_imu_cal_t;

typedef enum {
    RG_IMU_FWD_IDLE = 0,
    RG_IMU_FWD_ARMED,   // ждём разгон
    RG_IMU_FWD_DONE,
    RG_IMU_FWD_TIMEOUT, // разгона не было
} roundGauge_imu_fwd_t;

typedef struct {
    bool ok;               // датчик найден и отвечает
    bool calibrated;
    uint8_t fwd;           // выбранный вариант "вперёд", 0..3
    roundGauge_imu_cal_t cal;
    roundGauge_imu_fwd_t fwd_detect;
    float raw[3];          // сглаженное ускорение в осях платы, g
    float lon, lat, vert, tot;
} roundGauge_imu_state_t;

// Создаёт задачу. Вызывать после roundGauge_board_init() и roundGauge_settings_init().
void roundGauge_imu_task_start(void);

void roundGauge_imu_get_state(roundGauge_imu_state_t *out);

// Запомнить "вверх": машина должна стоять неподвижно около секунды. Не блокирует;
// итог - в поле cal.
esp_err_t roundGauge_imu_calibrate(void);

// Выбрать "вперёд" вручную (0..3) и сохранить.
esp_err_t roundGauge_imu_set_forward(uint8_t fwd);

// Определить "вперёд" по разгону: следующее заметное горизонтальное ускорение (около
// 0,12 g дольше 0,2 с) считается направлением вперёд. Итог - в поле fwd_detect.
esp_err_t roundGauge_imu_detect_forward(void);
