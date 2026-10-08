idf_component_register(SRCS "sound_task.c" "sound_logic.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos board config signals esp_timer)
