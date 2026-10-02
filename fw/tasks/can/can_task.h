#pragma once

// Приём CAN (TWAI, ESP-IDF v6: esp_twai_onchip).
//
// Путь кадра: колбэк on_rx_done из ISR -> очередь RG_CAN_RX_QUEUE_LEN -> can_task,
// которая разбирает сигналы по таблице из настроек. ISR только кладёт кадр в
// очередь: поиск по таблице и арифметика с float в прерывании не нужны. Из той
// же задачи веб будет получать кадры для сниффера.
//
// Режим (только прослушивание / нормальный) и скорость берутся из
// roundGauge_settings_get()->can.

// Создаёт очередь и задачу. Вызывать после roundGauge_settings_init().
void roundGauge_can_task_start(void);
