#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// =============================================================================
// stt — Speech-To-Text local via espressif/esp-sr (MultiNet)
//
// Recibe chunks de audio mono 16 kHz 16-bit desde el componente mic y
// corre el reconocimiento de comandos on-device sin WiFi ni latencia de red.
//
// Comandos predefinidos (IDs 1–5):
//   1  "stop"         — detener acción actual
//   2  "turn left"    — girar a la izquierda
//   3  "turn right"   — girar a la derecha
//   4  "alert on"     — activar alerta de emergencia
//   5  "help"         — modo de emergencia / llamada de auxilio
//
// Para agregar comandos propios: editar s_commands[] en stt.c y recompilar.
//
// Requisitos de build:
//   - Partición "model" de 5 MB en flash (ver partitions.csv)
//   - Componente espressif/esp-sr en components/stt/idf_component.yml
//   - idf.py flash para volcar los modelos a la partición model
// =============================================================================

#define STT_COMMAND_TEXT_MAX  64

typedef struct {
    int  command_id;
    char text[STT_COMMAND_TEXT_MAX];
} stt_result_t;

// Callback invocada cada vez que se reconoce un comando.
typedef void (*stt_result_cb_t)(const stt_result_t *result);

// Inicializa ESP-SR (carga modelos desde la partición "model").
// result_cb: función que recibirá cada resultado reconocido.
// Bloquea hasta que los modelos estén cargados (~1 segundo).
esp_err_t stt_init(stt_result_cb_t result_cb);

// Procesa un chunk de audio mono (16 kHz, 16-bit).
// count debe ser exactamente MIC_CHUNK_SAMPLES (480).
// Llamar desde la callback del componente mic.
void stt_feed(const int16_t *mono_samples, size_t count);

// Libera los modelos ESP-SR.
void stt_deinit(void);
