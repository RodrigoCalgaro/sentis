idf_component_register(
    SRCS "tts.c"
    INCLUDE_DIRS "."
    REQUIRES audio espeak-ng freertos
)
