#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// TTS — síntesis de voz en español vía eSpeak-NG.
//
// Flujo:
//   tts_init()  → monta la partición de flash "voice_data" (imagen FAT de
//                 solo lectura, ver components/tts/CMakeLists.txt y
//                 components/tts/voice_data/), configura voz "es"
//   tts_speak() → sintetiza texto UTF-8 → escribe PCM 16kHz → ES8311 → parlante
//
// Requiere:
//   - audio_init() ejecutado antes (I2S0 + ES8311 inicializados)
//
// Fase 2 (ver sentis-stability-integration-plan.md): los datos de voz se
// migraron de la SD a una partición de flash — ya no hace falta
// storage_init() ni una tarjeta SD para que TTS funcione. Solo se
// empaquetan los archivos necesarios para español (~718 KB), no el árbol
// completo de eSpeak-NG (~12 MB con ~100 idiomas que este proyecto no usa).
// =============================================================================

// Inicializa el motor eSpeak-NG (monta la partición de flash internamente)
// y selecciona la voz española.
// Retorna ESP_OK si la inicialización fue exitosa.
esp_err_t tts_init(void);

// Sintetiza text (UTF-8) y lo reproduce por el parlante.
// Bloqueante — retorna cuando la reproducción termina.
// Requiere tts_init() exitoso.
esp_err_t tts_speak(const char *text);

#ifdef __cplusplus
}
#endif
