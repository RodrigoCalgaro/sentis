idf_component_register(
    SRCS "ocr.cpp" "ocr_preprocess.c"
    INCLUDE_DIRS "."
    REQUIRES vision tts pp_ocr_v6 esp-dl freertos
)
