idf_component_register(SRCS "settings.c" "auth.c" "layout.c" "can_map.c" "alerts.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash freertos espressif__cjson)
