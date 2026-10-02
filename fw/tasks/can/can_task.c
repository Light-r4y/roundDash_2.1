#include "can_task.h"
#include <inttypes.h>
#include "conf.h"
#include "settings.h"
#include "board.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "CAN";

// Кадр в очереди: копия заголовка и данных, буфер драйвера переиспользуется.
typedef struct {
    twai_frame_header_t header;
    uint8_t data[TWAI_FRAME_MAX_LEN];
} can_rx_frame_t;

static QueueHandle_t s_rx_queue = NULL;

// TODO: узел TWAI.
//  - twai_new_node_onchip(): io_cfg.tx = RG_PIN_CAN_TX, io_cfg.rx = RG_PIN_CAN_RX,
//    bit_timing.bitrate = settings->can.bitrate,
//    flags.enable_listen_only = (settings->can.mode == RG_CAN_MODE_LISTEN_ONLY);
//  - on_rx_done (IRAM_ATTR): twai_node_receive_from_isr() + xQueueSendFromISR(s_rx_queue);
//  - on_state_change: при Bus-Off выставить флаг, восстановление — из задачи
//    (twai_node_recover() из ISR вызывать нельзя);
//  - twai_node_enable().

static void can_task(void *arg)
{
    const roundGauge_settings_t *cfg = roundGauge_settings_get();
    ESP_LOGI(TAG, "CAN task started (%" PRIu32 " bit/s, %s) - TWAI node: TODO",
             cfg->can.bitrate, cfg->can.mode == RG_CAN_MODE_LISTEN_ONLY ? "listen-only" : "normal");

    can_rx_frame_t frame;
    while (1) {
        if (xQueueReceive(s_rx_queue, &frame, portMAX_DELAY) == pdTRUE) {
            // TODO: разбор сигналов по таблице из настроек, обновление значений
            //       для ui_task, копия кадра в сниффер для веба.
        }
    }
}

void roundGauge_can_task_start(void)
{
    s_rx_queue = xQueueCreate(RG_CAN_RX_QUEUE_LEN, sizeof(can_rx_frame_t));
    configASSERT(s_rx_queue != NULL);

    BaseType_t ok = xTaskCreatePinnedToCore(can_task, "can_task", RG_CAN_TASK_STACK, NULL,
                                            RG_CAN_TASK_PRIORITY, NULL, RG_CAN_TASK_CORE);
    configASSERT(ok == pdPASS);
}
