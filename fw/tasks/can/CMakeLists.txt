idf_component_register(SRCS "can_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos esp_driver_twai board config signals)
