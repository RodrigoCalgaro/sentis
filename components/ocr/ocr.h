#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ocr — lectura de texto on-device (esp-dl pp_ocr_v6) con salida por voz.
//
// Flujo:
//   ocr_init()            → carga detector + reconocedor .espdl (una sola
//                           vez, en boot) y crea la tarea de lectura,
//                           inicialmente inactiva.
//   ocr_reading_start()   → señal no bloqueante: despierta la tarea y arranca
//                           el loop captura→detecta→reconoce→tts_speak().
//   ocr_reading_stop()    → señal no bloqueante: pide detener el loop en el
//                           próximo punto de interrupción seguro. La locución
//                           en curso (tts_speak) termina normalmente — no se
//                           cancela a mitad de frase.
//
// La ruta de los modelos en la SD NO es un parámetro en runtime: pp_ocr_v6
// (componente vendorizado espressif/pp_ocr_v6) construye el path internamente
// a partir de Kconfig — "menuconfig → Component config → models: pp_ocr_v6 →
// model location = sdcard", directorio por defecto "models/p4" relativo al
// punto de montaje (ver sdcard_files/README.md y sdkconfig.defaults).
//
// Requiere storage_init() (modelos en SD), vision_init() (captura de frames)
// y tts_init() (locución) completados antes de llamar ocr_init().
//
// Todas las funciones son seguras de llamar aunque ocr_init() no haya
// corrido o haya fallado (no-op).
// =============================================================================

// Carga los modelos pp_ocr_v6 (detector + reconocedor) desde la SD (ruta fija
// por Kconfig, ver arriba) y crea la tarea de lectura en espera.
// No fatal para el resto del sistema si falla: logea el error y retorna.
esp_err_t ocr_init(void);

// Arranca el loop de lectura. Idempotente: no-op si ya está leyendo.
// No bloqueante — seguro de llamar desde mic_task (callback de stt).
void ocr_reading_start(void);

// Pide detener el loop de lectura. Idempotente: no-op si ya está detenido.
// No bloqueante. El efecto puede demorar hasta una locución + una pasada de
// inferencia (ver nota en ocr.c).
void ocr_reading_stop(void);

// true si el loop de lectura está activo en este momento.
bool ocr_is_reading(void);

#ifdef __cplusplus
}
#endif
