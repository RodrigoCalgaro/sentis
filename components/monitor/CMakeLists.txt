idf_component_register(
    SRCS "monitor.c"
    INCLUDE_DIRS "."
    REQUIRES vision esp_driver_jpeg esp_driver_uart freertos
)
