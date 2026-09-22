idf_component_register(
    SRCS "sentis.c"
    INCLUDE_DIRS "."
    REQUIRES haptics wifi cp_ota link lidar vision monitor audio mic tts ocr settings freertos board
)
