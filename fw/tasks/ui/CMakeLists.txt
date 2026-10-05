idf_component_register(SRCS "ui_task.c" "ui_screens.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos spiffs board config can signals esp_timer webcfg)
