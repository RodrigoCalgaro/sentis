idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2s esp_driver_gpio bus freertos board
)
