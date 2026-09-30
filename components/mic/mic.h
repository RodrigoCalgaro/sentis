#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

// =============================================================================
// mic — captura continua desde el micrófono externo INMP441 (I2S1 RX)
//
// Lee el canal izquierdo del INMP441 (24 bit en slot de 32) y lo convierte a int16 mono.
// Entrega chunks de MIC_CHUNK_SAMPLES muestras a la callback registrada.
// El tamaño del chunk es el frame que espera ESP-SR: 30 ms @ 16 kHz = 480 samples.
// =============================================================================

#define MIC_CHUNK_SAMPLES  480    // 30 ms @ 16 kHz, mono
#define MIC_SAMPLE_RATE    16000  // 16 kHz, igual que el resto del pipeline

// Callback invocada desde la tarea de captura cada vez que hay un chunk listo.
// samples : MIC_CHUNK_SAMPLES muestras int16 mono, buffer válido solo durante
//           la duración de la llamada — copiar si se necesita retención.
typedef void (*mic_data_cb_t)(const int16_t *samples, size_t count);

// Inicia la tarea de captura de micrófono.
// data_cb: función que recibirá cada chunk de audio (no puede ser NULL).
// Independiente de audio_init(): usa su propio periférico I2S1.
esp_err_t mic_init(mic_data_cb_t data_cb);

// Detiene la tarea y libera recursos.
void mic_deinit(void);

// Retorna true si la tarea de captura está corriendo.
bool mic_is_running(void);
