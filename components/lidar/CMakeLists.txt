idf_component_register(
    SRCS "lidar.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_uart freertos board
)
