idf_component_register(
    SRCS "sentis.c"
    INCLUDE_DIRS "."
    REQUIRES haptics wifi cp_ota link lidar vision monitor storage audio mic tts ocr freertos board
)
