idf_component_register(SRCS "ui_task.c" "test_screen.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos spiffs board config can
                    PRIV_REQUIRES esp_app_format)
