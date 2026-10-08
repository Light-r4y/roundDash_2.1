idf_component_register(SRCS "webcfg_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos esp_wifi esp_netif esp_event esp_http_server spiffs config can board imu sound
                    PRIV_REQUIRES app_update esp_partition spiffs esp_timer)
