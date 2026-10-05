#include "can_task.h"
#include <inttypes.h>
#include <string.h>
#include "conf.h"
#include "settings.h"
#include "can_map.h"
#include "signals.h"
#include "board.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "CAN";

// Кадр в очереди: только то, что нужно разбору.
typedef struct {
    uint32_t id;
    uint8_t ext;
    uint8_t dlc;
    uint8_t data[8];
} can_rx_frame_t;

static QueueHandle_t s_rx_queue = NULL;
static TaskHandle_t s_task = NULL;
static twai_node_handle_t s_node = NULL;

// ---------------------------------------------------------------------------
// Фильтр ID для ISR: какие кадры вообще нужны. Таблица меняется из can_task
// редко, поэтому два буфера и переключение указателя - ISR читает без блокировок.
// ---------------------------------------------------------------------------
typedef struct {
    uint8_t count;
    uint32_t key[RG_CAN_MAP_MAX]; // id | (ext << 31)
} id_filter_t;

static id_filter_t s_filter_buf[2];
static const id_filter_t *volatile s_filter = &s_filter_buf[0];
static volatile TickType_t s_sniff_until; // до этого тика принимаем и чужие ID

static inline bool filter_has(const id_filter_t *f, uint32_t key)
{
    for (int i = 0; i < f->count; i++) {
        if (f->key[i] == key) {
            return true;
        }
    }
    return false;
}

// Счётчики (пишет ISR или can_task, читает веб - небольшая неточность не страшна).
static volatile uint32_t s_stat_rx, s_stat_dropped, s_stat_busoff;
static volatile uint8_t s_err_state;
static volatile bool s_busoff;

static bool IRAM_ATTR on_rx_done(twai_node_handle_t handle, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    can_rx_frame_t item;
    memset(item.data, 0, sizeof(item.data));
    twai_frame_t frame = { .buffer = item.data, .buffer_len = sizeof(item.data) };
    if (twai_node_receive_from_isr(handle, &frame) != ESP_OK) {
        return false;
    }
    s_stat_rx++;
    if (frame.header.rtr || frame.header.fdf) {
        return false;
    }
    item.id = frame.header.id;
    item.ext = frame.header.ide ? 1 : 0;
    item.dlc = frame.header.dlc > 8 ? 8 : (uint8_t)frame.header.dlc;

    bool sniffing = (int32_t)(s_sniff_until - xTaskGetTickCountFromISR()) > 0;
    if (!sniffing && !filter_has(s_filter, item.id | ((uint32_t)item.ext << 31))) {
        return false;
    }
    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_rx_queue, &item, &woken) != pdTRUE) {
        s_stat_dropped++;
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_state_change(twai_node_handle_t handle, const twai_state_change_event_data_t *edata,
                                      void *user_ctx)
{
    s_err_state = (uint8_t)edata->new_sta;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_busoff = true;
        s_stat_busoff++;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Сниффер: последний кадр по каждому ID. Пишет can_task, читает httpd - спинлок на
// копирование нескольких байт.
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t id;
    uint8_t ext, dlc;
    uint8_t data[8];
    uint32_t count;
    uint32_t last_ms;
} sniff_entry_t;

static sniff_entry_t s_sniff[RG_CAN_SNIFFER_MAX];
static int s_sniff_count;
static portMUX_TYPE s_sniff_lock = portMUX_INITIALIZER_UNLOCKED;

static inline uint32_t tick_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

static void sniff_update(const can_rx_frame_t *f)
{
    portENTER_CRITICAL(&s_sniff_lock);
    sniff_entry_t *e = NULL;
    for (int i = 0; i < s_sniff_count; i++) {
        if (s_sniff[i].id == f->id && s_sniff[i].ext == f->ext) {
            e = &s_sniff[i];
            break;
        }
    }
    if (e == NULL && s_sniff_count < RG_CAN_SNIFFER_MAX) {
        e = &s_sniff[s_sniff_count++];
        e->id = f->id;
        e->ext = f->ext;
        e->count = 0;
    }
    if (e != NULL) {
        e->dlc = f->dlc;
        memcpy(e->data, f->data, 8);
        e->count++;
        e->last_ms = tick_ms();
    }
    portEXIT_CRITICAL(&s_sniff_lock);
}

size_t roundGauge_can_sniffer_snapshot(roundGauge_can_frame_info_t *out, size_t max)
{
    s_sniff_until = xTaskGetTickCount() + pdMS_TO_TICKS(RG_CAN_SNIFFER_ACTIVE_MS);
    uint32_t now = tick_ms();
    size_t n = 0;
    portENTER_CRITICAL(&s_sniff_lock);
    for (int i = 0; i < s_sniff_count && n < max; i++, n++) {
        out[n].id = s_sniff[i].id;
        out[n].ext = s_sniff[i].ext;
        out[n].dlc = s_sniff[i].dlc;
        memcpy(out[n].data, s_sniff[i].data, 8);
        out[n].count = s_sniff[i].count;
        out[n].age_ms = now - s_sniff[i].last_ms;
    }
    portEXIT_CRITICAL(&s_sniff_lock);
    return n;
}

// ---------------------------------------------------------------------------
// Таблица привязок
// ---------------------------------------------------------------------------
static roundGauge_can_map_t s_cmap;
static int s_sig_id[RG_CAN_MAP_MAX];
static uint32_t s_map_gen;
static bool s_demo;

static void map_rebuild(void)
{
    // Сигналы, которые были привязаны, а теперь нет, не должны остаться "живыми".
    int old_ids[RG_CAN_MAP_MAX];
    int old_count = s_cmap.count;
    memcpy(old_ids, s_sig_id, sizeof(old_ids));

    s_map_gen = roundGauge_can_map_generation();
    roundGauge_can_map_copy(&s_cmap);

    static int fi; // какой из двух буферов фильтра сейчас пишем
    fi ^= 1;
    id_filter_t *f = &s_filter_buf[fi];
    f->count = 0;
    for (int i = 0; i < s_cmap.count; i++) {
        const roundGauge_can_entry_t *e = &s_cmap.e[i];
        s_sig_id[i] = roundGauge_signal_id(e->signal);
        roundGauge_signal_set_timeout(s_sig_id[i], e->timeout_ms);
        uint32_t key = e->id | ((uint32_t)e->ext << 31);
        if (!filter_has(f, key) && f->count < RG_CAN_MAP_MAX) {
            f->key[f->count++] = key;
        }
    }
    s_filter = f;

    for (int i = 0; i < old_count; i++) {
        bool still = false;
        for (int j = 0; j < s_cmap.count; j++) {
            still |= s_sig_id[j] == old_ids[i];
        }
        if (!still) {
            roundGauge_signal_set_timeout(old_ids[i], 0);
            roundGauge_signal_invalidate(old_ids[i]);
        }
    }
    ESP_LOGI(TAG, "CAN map: %u entries, %u ids to accept", (unsigned)s_cmap.count, (unsigned)f->count);
}

static void process_frame(const can_rx_frame_t *f)
{
    if ((int32_t)(s_sniff_until - xTaskGetTickCount()) > 0) {
        sniff_update(f);
    }
    if (s_demo) {
        return; // значения гонит генератор
    }
    for (int i = 0; i < s_cmap.count; i++) {
        const roundGauge_can_entry_t *e = &s_cmap.e[i];
        if (e->id != f->id || e->ext != f->ext) {
            continue;
        }
        int64_t raw;
        if (!roundGauge_can_extract(f->data, e->start, e->len, e->order, e->is_signed, &raw)) {
            continue;
        }
        roundGauge_signal_set(s_sig_id[i], (float)raw * e->factor + e->offset);
    }
}

// ---------------------------------------------------------------------------
// Узел TWAI
// ---------------------------------------------------------------------------
static bool s_node_up;
static uint16_t s_tx_err, s_rx_err;
static uint32_t s_bus_err;

static void node_stop(void)
{
    if (s_node != NULL) {
        twai_node_disable(s_node);
        twai_node_delete(s_node);
        s_node = NULL;
    }
    s_node_up = false;
}

static void node_start(const roundGauge_can_settings_t *c)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = RG_PIN_CAN_TX,
            .rx = RG_PIN_CAN_RX,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = { .bitrate = c->bitrate },
        .fail_retry_cnt = 3,
        .tx_queue_depth = 1, // передавать мы не собираемся, но драйверу нужна хотя бы 1
        .intr_priority = 0,
        .flags = {
            .enable_listen_only = c->mode == RG_CAN_MODE_LISTEN_ONLY,
            .no_receive_rtr = 1,
        },
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_new_node_onchip(%" PRIu32 ") failed: %s", c->bitrate, esp_err_to_name(err));
        s_node = NULL;
        return;
    }
    const twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
    };
    ESP_ERROR_CHECK(twai_node_register_event_callbacks(s_node, &cbs, NULL));
    err = twai_node_enable(s_node);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "twai_node_enable failed: %s", esp_err_to_name(err));
        twai_node_delete(s_node);
        s_node = NULL;
        return;
    }
    s_err_state = 0;
    s_busoff = false;
    s_node_up = true;
    ESP_LOGI(TAG, "CAN node up: %" PRIu32 " bit/s, %s", c->bitrate,
             c->mode == RG_CAN_MODE_LISTEN_ONLY ? "listen-only" : "normal (ACK)");
}

static void apply_settings(void)
{
    roundGauge_can_settings_t c;
    roundGauge_settings_get_can(&c);
    s_demo = c.demo;
    roundGauge_signal_sim_enable(c.demo);
    if (c.demo) {
        ESP_LOGW(TAG, "Demo mode: signals come from the generator");
    }
    node_stop();
    xQueueReset(s_rx_queue);
    node_start(&c);
}

void roundGauge_can_apply_settings(void)
{
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
}

void roundGauge_can_get_status(roundGauge_can_status_t *out)
{
    out->node_up = s_node_up;
    out->state = s_err_state;
    out->rx = s_stat_rx;
    out->dropped = s_stat_dropped;
    out->tx_err = s_tx_err;
    out->rx_err = s_rx_err;
    out->bus_err = s_bus_err;
    out->busoff_count = s_stat_busoff;
}

static void poll_node_info(void)
{
    if (s_node == NULL) {
        return;
    }
    twai_node_status_t st;
    twai_node_record_t rec;
    if (twai_node_get_info(s_node, &st, &rec) == ESP_OK) {
        s_tx_err = st.tx_error_count;
        s_rx_err = st.rx_error_count;
        s_bus_err = rec.bus_err_num;
        s_err_state = (uint8_t)st.state;
    }
}

static void can_task(void *arg)
{
    map_rebuild();
    apply_settings();

    TickType_t busoff_since = 0;
    TickType_t last_poll = xTaskGetTickCount();
    can_rx_frame_t frame;
    while (1) {
        if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(100)) == pdTRUE) {
            process_frame(&frame);
            // Хвост очереди разбираем сразу, не возвращаясь в ожидание.
            while (xQueueReceive(s_rx_queue, &frame, 0) == pdTRUE) {
                process_frame(&frame);
            }
        }

        if (ulTaskNotifyTake(pdTRUE, 0) > 0) {
            apply_settings();
        }
        if (roundGauge_can_map_generation() != s_map_gen) {
            map_rebuild();
        }

        // Bus-off: восстанавливаемся из задачи (из ISR twai_node_recover() вызывать нельзя).
        TickType_t now = xTaskGetTickCount();
        if (s_busoff && s_node != NULL) {
            if (busoff_since == 0) {
                busoff_since = now;
                ESP_LOGW(TAG, "Bus-off, recovering in %d ms", RG_CAN_BUSOFF_RECOVERY_MS);
            } else if ((now - busoff_since) >= pdMS_TO_TICKS(RG_CAN_BUSOFF_RECOVERY_MS)) {
                s_busoff = false;
                busoff_since = 0;
                twai_node_recover(s_node);
            }
        } else {
            busoff_since = 0;
        }

        if ((now - last_poll) >= pdMS_TO_TICKS(500)) {
            last_poll = now;
            poll_node_info();
        }
    }
}

void roundGauge_can_task_start(void)
{
    s_rx_queue = xQueueCreate(RG_CAN_RX_QUEUE_LEN, sizeof(can_rx_frame_t));
    configASSERT(s_rx_queue != NULL);

    BaseType_t ok = xTaskCreatePinnedToCore(can_task, "can_task", RG_CAN_TASK_STACK, NULL,
                                            RG_CAN_TASK_PRIORITY, &s_task, RG_CAN_TASK_CORE);
    configASSERT(ok == pdPASS);
}
