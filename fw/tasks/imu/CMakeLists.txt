idf_component_register(SRCS "imu_task.c" "imu_math.c"
                    INCLUDE_DIRS "."
                    REQUIRES freertos esp_driver_i2c board config signals esp_timer)
