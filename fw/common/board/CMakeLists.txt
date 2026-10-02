idf_component_register(SRCS "board.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_lcd espressif__esp_lcd_touch
                    PRIV_REQUIRES esp_driver_i2c esp_driver_ledc)
