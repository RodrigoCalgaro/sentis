idf_component_register(
    SRCS "cp_ota.c"
    INCLUDE_DIRS "."
    REQUIRES espressif__esp_hosted esp_partition bootloader_support esp_app_format
)
