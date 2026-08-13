#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// TTS — síntesis de voz en español vía eSpeak-NG.
//
// Flujo:
//   tts_init()  → carga datos desde SD, configura voz "es"
//   tts_speak() → sintetiza texto UTF-8 → escribe PCM 16kHz → ES8311 → parlante
//
// Requiere:
//   - storage_init() ejecutado antes (SD montada en /sdcard)
//   - audio_init()   ejecutado antes (I2S0 + ES8311 inicializados)
//   - Archivos en SD: /sdcard/espeak-ng-data/  (ver sdcard_files/README.md)
// =============================================================================

// Inicializa el motor eSpeak-NG y selecciona la voz española.
// data_path: ruta VFS al directorio de datos (p. ej. "/sdcard/espeak-ng-data").
// Retorna ESP_OK si la inicialización fue exitosa.
esp_err_t tts_init(const char *data_path);

// Sintetiza text (UTF-8) y lo reproduce por el parlante.
// Bloqueante — retorna cuando la reproducción termina.
// Requiere tts_init() exitoso.
esp_err_t tts_speak(const char *text);

#ifdef __cplusplus
}
#endif
