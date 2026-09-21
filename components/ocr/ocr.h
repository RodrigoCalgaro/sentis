#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ocr — lectura de texto vía la app Android companion (ML Kit del lado del
// celular), con salida por voz en el propio ESP32.
//
// Fase 2 (ver sentis-stability-integration-plan.md): este componente ya NO
// corre inferencia on-device — la inferencia real (antes esp-dl/pp_ocr_v6,
// retirada por crashes y baja precisión) se retiró por completo. Ahora solo
// captura un frame, lo codifica a JPEG por hardware (esp_driver_jpeg) y lo
// manda por components/link a la app, que responde con el texto reconocido.
//
// Flujo:
//   ocr_init()            → arma los buffers de captura/JPEG (una sola vez,
//                           en boot) y crea la tarea de lectura, inicialmente
//                           inactiva. Ya no depende de la SD.
//   ocr_reading_start()   → señal no bloqueante: despierta la tarea y arranca
//                           el loop captura→JPEG→link_request_ocr_text()→tts_speak().
//   ocr_reading_stop()    → señal no bloqueante: pide detener el loop en el
//                           próximo punto de interrupción seguro. La locución
//                           en curso (tts_speak) termina normalmente — no se
//                           cancela a mitad de frase.
//
// Requiere vision_init() (captura de frames), tts_init() (locución) y
// link_init() (transporte hacia la app) completados antes de llamar
// ocr_init() — o al menos antes del primer "start reading" real.
//
// Todas las funciones son seguras de llamar aunque ocr_init() no haya
// corrido o haya fallado (no-op).
// =============================================================================

// Arma los buffers de captura+JPEG y crea la tarea de lectura en espera.
// No fatal para el resto del sistema si falla: logea el error y retorna.
esp_err_t ocr_init(void);

// Arranca el loop de lectura. Idempotente: no-op si ya está leyendo.
// No bloqueante — seguro de llamar desde la tarea de recepción de link
// (callback de comando, ver on_link_command en main/sentis.c).
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
