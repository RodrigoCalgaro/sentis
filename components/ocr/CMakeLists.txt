idf_component_register(
    SRCS "ocr.cpp"
    INCLUDE_DIRS "."
    REQUIRES vision tts link esp_driver_jpeg freertos
)
