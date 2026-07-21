idf_component_register(
    SRCS "storage.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_sdmmc sdmmc fatfs esp_hw_support board
)
