idf_component_register(SRCS "touch_task.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos board config ui espressif__esp_lcd_touch)
