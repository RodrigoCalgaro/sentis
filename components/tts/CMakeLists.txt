idf_component_register(
    SRCS "tts.c"
    INCLUDE_DIRS "."
    REQUIRES audio espeak-ng freertos fatfs
)

# Empaqueta components/tts/voice_data/ (datos de voz eSpeak-NG, solo
# espanol, ver ese directorio y partitions.csv) como una imagen FAT de solo
# lectura en la particion "voice_data", generada y flasheada automaticamente
# por idf.py build/flash (FLASH_IN_PROJECT). Sin wear-levelling (raw, no
# spiflash con WL) porque los datos nunca se escriben en tiempo de
# ejecucion -- pareja con esp_vfs_fat_spiflash_mount_ro() en tts.c.
fatfs_create_rawflash_image(voice_data "${CMAKE_CURRENT_SOURCE_DIR}/voice_data" FLASH_IN_PROJECT)
