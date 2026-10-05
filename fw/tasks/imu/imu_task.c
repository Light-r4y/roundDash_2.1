#include "imu_task.h"
#include <math.h>
#include <string.h>
#include "conf.h"
#include "board.h"
#include "settings.h"
#include "signals.h"
#include "imu_math.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "IMU";

// QMI8658: регистры (даташит hw/datasheets/QMI8658A.pdf).
#define REG_WHO_AM_I 0x00 // 0x05
#define REG_CTRL1 0x02    // 0x40: автоинкремент адреса, данные little-endian, генератор включён
#define REG_CTRL2 0x03    // ускорение: диапазон [6:4], частота [3:0]
#define REG_CTRL7 0x08    // bit0 aEN - включить акселерометр
#define REG_AX_L 0x35     // AX_L AX_H AY_L AY_H AZ_L AZ_H, знаковые 16 бит
#define CTRL2_4G_125HZ 0x16 // +-4 g (8192 LSB/g), 125 Гц, только акселерометр
#define LSB_PER_G 8192.0f

#define POLL_MS 8                 // 125 Гц - как частота выдачи данных датчиком
#define LPF_ALPHA 0.33f           // сглаживание для экрана: ~7 Гц при 125 Гц
#define CAL_SAMPLES 125           // ~1 с неподвижности
#define CAL_MAX_SPREAD_G 0.16f    // размах отсчётов за калибровку больше - машина двигалась
#define FWD_MIN_G 0.12f           // горизонтальное ускорение, считающееся разгоном
#define FWD_MIN_SAMPLES 25        // дольше ~0,2 с подряд
#define FWD_TIMEOUT_MS 15000
#define FAIL_LIMIT 20             // столько ошибок чтения подряд - датчик считаем пропавшим
#define RETRY_MS 2000

static i2c_master_dev_handle_t s_dev;
static roundGauge_imu_state_t s_state;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_cal_req, s_fwd_req;

// ---------------------------------------------------------------------------
// Датчик
// ---------------------------------------------------------------------------

static esp_err_t reg_write(uint8_t reg, uint8_t value)
{
    uint8_t b[2] = { reg, value };
    return i2c_master_transmit(s_dev, b, sizeof(b), 50);
}

static esp_err_t reg_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 50);
}

// Вернуть ошибку из функции, если вызов не удался.
#define TRY(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) return e_; } while (0)

static esp_err_t sensor_init(void)
{
    if (s_dev == NULL) {
        i2c_device_config_t cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = RG_I2C_ADDR_IMU,
            .scl_speed_hz = 400000,
        };
        esp_err_t err = i2c_master_bus_add_device(roundGauge_board_i2c(), &cfg, &s_dev);
        if (err != ESP_OK) {
            s_dev = NULL;
            return err;
        }
    }
    uint8_t id = 0;
    esp_err_t err = reg_read(REG_WHO_AM_I, &id, 1);
    if (err != ESP_OK) {
        return err;
    }
    if (id != 0x05) {
        ESP_LOGW(TAG, "WHO_AM_I = 0x%02X, expected 0x05", id);
        return ESP_FAIL;
    }
    // Сначала всё выключить, потом настроить и включить акселерометр.
    TRY(reg_write(REG_CTRL7, 0x00));
    vTaskDelay(pdMS_TO_TICKS(10));
    TRY(reg_write(REG_CTRL1, 0x40));
    TRY(reg_write(REG_CTRL2, CTRL2_4G_125HZ));
    TRY(reg_write(REG_CTRL7, 0x01));
    vTaskDelay(pdMS_TO_TICKS(30)); // первые данные
    return ESP_OK;
}

// Ускорение в g, в осях платы.
static esp_err_t sensor_read(float a[3])
{
    uint8_t b[6];
    esp_err_t err = reg_read(REG_AX_L, b, sizeof(b));
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < 3; i++) {
        int16_t raw = (int16_t)((uint16_t)b[2 * i] | ((uint16_t)b[2 * i + 1] << 8));
        a[i] = (float)raw / LSB_PER_G;
    }
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

void roundGauge_imu_get_state(roundGauge_imu_state_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_state;
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t roundGauge_imu_calibrate(void)
{
    if (!s_state.ok) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cal_req = true;
    return ESP_OK;
}

esp_err_t roundGauge_imu_detect_forward(void)
{
    if (!s_state.ok) {
        return ESP_ERR_INVALID_STATE;
    }
    s_fwd_req = true;
    return ESP_OK;
}

esp_err_t roundGauge_imu_set_forward(uint8_t fwd)
{
    if (fwd > 3) {
        return ESP_ERR_INVALID_ARG;
    }
    roundGauge_imu_settings_t s;
    roundGauge_settings_get_imu(&s);
    s.fwd = fwd;
    return roundGauge_settings_set_imu(&s);
}

// ---------------------------------------------------------------------------
// Задача
// ---------------------------------------------------------------------------

static void publish_state(const roundGauge_imu_state_t *st)
{
    portENTER_CRITICAL(&s_lock);
    s_state = *st;
    portEXIT_CRITICAL(&s_lock);
}

static void imu_task(void *arg)
{
    const int id_lon = roundGauge_signal_id(RG_SIG_G_LON);
    const int id_lat = roundGauge_signal_id(RG_SIG_G_LAT);
    const int id_vert = roundGauge_signal_id(RG_SIG_G_VERT);
    const int id_tot = roundGauge_signal_id(RG_SIG_G_TOT);
    const int ids[4] = { id_lon, id_lat, id_vert, id_tot };
    for (int i = 0; i < 4; i++) {
        roundGauge_signal_set_timeout(ids[i], RG_IMU_SIGNAL_TIMEOUT_MS);
        roundGauge_signal_sim_exempt(ids[i]);
    }

    roundGauge_imu_state_t st = {0};
    float filt[3] = {0};
    bool have_filt = false;
    int fails = 0;
    int64_t next_retry_ms = 0;

    // Калибровка по гравитации.
    int cal_n = 0;
    float cal_sum[3] = {0}, cal_min[3], cal_max[3];
    // Определение "вперёд" по разгону.
    int fwd_run = 0;
    float fwd_sum[3] = {0};
    int64_t fwd_deadline_ms = 0;

    TickType_t last = xTaskGetTickCount();
    while (1) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(POLL_MS));
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (!st.ok) {
            if (now_ms >= next_retry_ms) {
                next_retry_ms = now_ms + RETRY_MS;
                if (sensor_init() == ESP_OK) {
                    st.ok = true;
                    fails = 0;
                    have_filt = false;
                    ESP_LOGI(TAG, "QMI8658 found, accelerometer on (+-4 g, 125 Hz)");
                } else {
                    ESP_LOGW(TAG, "QMI8658 not responding");
                }
            }
            publish_state(&st);
            continue;
        }

        float a[3];
        if (sensor_read(a) != ESP_OK) {
            if (++fails >= FAIL_LIMIT) {
                ESP_LOGW(TAG, "QMI8658 lost");
                st.ok = false;
                next_retry_ms = now_ms + RETRY_MS;
            }
            publish_state(&st);
            continue;
        }
        fails = 0;
        if (!have_filt) {
            memcpy(filt, a, sizeof(filt));
            have_filt = true;
        } else {
            for (int i = 0; i < 3; i++) filt[i] += LPF_ALPHA * (a[i] - filt[i]);
        }

        roundGauge_imu_settings_t cfg;
        roundGauge_settings_get_imu(&cfg);
        st.calibrated = cfg.calibrated;
        st.fwd = cfg.fwd;

        // --- калибровка: среднее за ~1 с по несглаженным отсчётам ---
        if (s_cal_req) {
            s_cal_req = false;
            cal_n = 0;
            memset(cal_sum, 0, sizeof(cal_sum));
            st.cal = RG_IMU_CAL_RUNNING;
        }
        if (st.cal == RG_IMU_CAL_RUNNING) {
            for (int i = 0; i < 3; i++) {
                if (cal_n == 0) cal_min[i] = cal_max[i] = a[i];
                cal_sum[i] += a[i];
                if (a[i] < cal_min[i]) cal_min[i] = a[i];
                if (a[i] > cal_max[i]) cal_max[i] = a[i];
            }
            if (++cal_n >= CAL_SAMPLES) {
                float dev = 0;
                for (int i = 0; i < 3; i++) dev = fmaxf(dev, cal_max[i] - cal_min[i]);
                if (dev > CAL_MAX_SPREAD_G) {
                    st.cal = RG_IMU_CAL_MOVED;
                    ESP_LOGW(TAG, "Calibration failed: the car was moving (spread %.3f g)", dev);
                } else {
                    cfg.calibrated = true;
                    for (int i = 0; i < 3; i++) cfg.g0[i] = cal_sum[i] / (float)cal_n;
                    if (roundGauge_settings_set_imu(&cfg) == ESP_OK) {
                        st.cal = RG_IMU_CAL_DONE;
                        st.calibrated = true;
                        ESP_LOGI(TAG, "Calibrated: g0 = (%.3f, %.3f, %.3f)", cfg.g0[0], cfg.g0[1], cfg.g0[2]);
                    } else {
                        st.cal = RG_IMU_CAL_MOVED;
                    }
                }
            }
        }

        // --- "вперёд" по разгону ---
        if (s_fwd_req) {
            s_fwd_req = false;
            st.fwd_detect = RG_IMU_FWD_ARMED;
            fwd_run = 0;
            memset(fwd_sum, 0, sizeof(fwd_sum));
            fwd_deadline_ms = now_ms + FWD_TIMEOUT_MS;
        }
        if (st.fwd_detect == RG_IMU_FWD_ARMED) {
            float d[3] = { filt[0] - cfg.g0[0], filt[1] - cfg.g0[1], filt[2] - cfg.g0[2] };
            float h[3];
            float hl = roundGauge_imu_horizontal(cfg.g0, d, h);
            if (hl > FWD_MIN_G) {
                for (int i = 0; i < 3; i++) fwd_sum[i] += h[i];
                if (++fwd_run >= FWD_MIN_SAMPLES) {
                    cfg.fwd = roundGauge_imu_pick_forward(cfg.g0, fwd_sum);
                    if (roundGauge_settings_set_imu(&cfg) == ESP_OK) {
                        st.fwd = cfg.fwd;
                        st.fwd_detect = RG_IMU_FWD_DONE;
                        ESP_LOGI(TAG, "Forward detected: variant %u", (unsigned)cfg.fwd);
                    }
                }
            } else {
                fwd_run = 0;
                memset(fwd_sum, 0, sizeof(fwd_sum));
            }
            if (st.fwd_detect == RG_IMU_FWD_ARMED && now_ms > fwd_deadline_ms) {
                st.fwd_detect = RG_IMU_FWD_TIMEOUT;
            }
        }

        // --- проекция на оси машины и сигналы ---
        memcpy(st.raw, filt, sizeof(st.raw));
        roundGauge_imu_project(cfg.g0, cfg.fwd, filt, &st.lon, &st.lat, &st.vert);
        st.tot = sqrtf(st.lon * st.lon + st.lat * st.lat);
        roundGauge_signal_set(id_lon, st.lon);
        roundGauge_signal_set(id_lat, st.lat);
        roundGauge_signal_set(id_vert, st.vert);
        roundGauge_signal_set(id_tot, st.tot);

        publish_state(&st);
    }
}

void roundGauge_imu_task_start(void)
{
    BaseType_t ok = xTaskCreatePinnedToCore(imu_task, "imu_task", RG_IMU_TASK_STACK, NULL, RG_IMU_TASK_PRIORITY, NULL,
                                            RG_IMU_TASK_CORE);
    configASSERT(ok == pdPASS);
}
