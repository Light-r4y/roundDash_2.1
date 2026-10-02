idf_component_register(SRCS "settings.c" "auth.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash freertos espressif__cjson)
