idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash app_update board config ui can buttons touch webcfg signals)
