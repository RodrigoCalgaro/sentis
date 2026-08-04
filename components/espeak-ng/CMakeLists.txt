# eSpeak-NG synthesis library for ESP32-P4
#
# Source files are NOT included in the repository — run the setup script first:
#   powershell -ExecutionPolicy Bypass -File scripts/setup_espeak.ps1
#
# Expected layout after setup:
#   src/libespeak-ng/  ← .c synthesis sources (exclude compile*.c)
#   include/espeak-ng/ ← espeak_ng.h, speak_lib.h, encoding.h

file(GLOB ALL_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/libespeak-ng/*.c")
# Exclude only true compilation-tool sources and CLI frontends.
# compiledict.c is NOT excluded — it contains runtime dictionary loading.
list(FILTER ALL_SRCS EXCLUDE REGEX "(compiledata|compilembrola|compilelex|compiledictionary|espeak-ng|mbrowrap|sPlayer)\\.c$")

if(NOT ALL_SRCS)
    message(FATAL_ERROR
        "\n"
        "========================================================\n"
        " eSpeak-NG source files not found.\n"
        " Run the setup script from the project root:\n"
        "   powershell -ExecutionPolicy Bypass -File scripts/setup_espeak.ps1\n"
        " Then rebuild.\n"
        "========================================================\n")
endif()

idf_component_register(
    SRCS    ${ALL_SRCS}
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
                 "${CMAKE_CURRENT_LIST_DIR}/include"
                 "${CMAKE_CURRENT_LIST_DIR}/src/libespeak-ng"
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC
    USE_ASYNC=0
    USE_MBROLA=0
    SAMPLE_RATE=16000
    HAVE_STDINT_H=1
    HAVE_STDBOOL_H=1
    HAVE_WCTYPE_H=1
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-parameter
    -Wno-maybe-uninitialized
    -Wno-implicit-function-declaration
    -Wno-sign-compare
    -Wno-format-truncation
    -Wno-format-overflow
    -Wno-stringop-truncation
    -Wno-stringop-overflow
    -Wno-restrict
    -Wno-error
)
