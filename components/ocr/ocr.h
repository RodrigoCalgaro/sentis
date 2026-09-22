#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ocr — captura de un frame + codificación JPEG por hardware + pedido/
// respuesta vía components/link hacia la app Android companion. Dos
// consumidores comparten el mismo capturador (mismo task, mismos buffers de
// PSRAM — no se duplica reserva de memoria):
//
//   - Lectura de texto (OCR): ML Kit reconoce el texto del lado del celular
//     y también lo locuta con el TTS nativo de Android (Fase 3) — este
//     componente no reproduce audio. Loop continuo mientras dura la lectura.
//   - Detección de color (ver ocr_detect_color()): un solo pedido/respuesta,
//     la app calcula el color dominante del frame (análisis HSV on-device,
//     sin IA) y lo locuta igual que el OCR.
//
// El nombre del componente/archivo sigue siendo "ocr" por historia — si en
// el futuro se suman más detecciones de un solo frame (ej. billetes), vale
// la pena evaluar un nombre más general para el componente.
//
// Fase 2 (ver sentis-stability-integration-plan.md): este componente ya NO
// corre inferencia on-device — la inferencia real de texto (antes
// esp-dl/pp_ocr_v6, retirada por crashes y baja precisión) se retiró por
// completo. Ahora solo captura, codifica a JPEG (esp_driver_jpeg) y manda
// por components/link a la app, que responde con el resultado.
//
// Flujo:
//   ocr_init()            → arma los buffers de captura/JPEG (una sola vez,
//                           en boot) y crea la tarea de captura, inicialmente
//                           inactiva. Ya no depende de la SD.
//   ocr_reading_start()   → señal no bloqueante: despierta la tarea y arranca
//                           el loop captura→JPEG→link_request_ocr_text() (la
//                           app locuta el resultado del lado del celular).
//   ocr_reading_stop()    → señal no bloqueante: pide detener el loop en el
//                           próximo punto de interrupción seguro.
//   ocr_detect_color()    → señal no bloqueante: despierta la tarea para un
//                           solo ciclo captura→JPEG→link_request_color().
//                           No-op (con log) si ya hay una lectura o una
//                           detección de color en curso — mismo criterio
//                           idempotente que ocr_reading_start().
//
// Requiere vision_init() (captura de frames) y link_init() (transporte hacia
// la app) completados antes de llamar ocr_init() — o al menos antes del
// primer "start reading"/"detectar color" real.
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
// No bloqueante. El efecto puede demorar hasta un link_request_ocr_text() en
// curso (ver OCR_REQUEST_TIMEOUT_MS en ocr.cpp).
void ocr_reading_stop(void);

// true si el loop de lectura está activo en este momento.
bool ocr_is_reading(void);

// Pide un solo ciclo de captura+JPEG+detección de color (ver arriba).
// No bloqueante — seguro de llamar desde la tarea de recepción de link
// (callback de comando, ver on_link_command en main/sentis.c). No-op si ya
// hay una lectura o una detección de color en curso.
void ocr_detect_color(void);

#ifdef __cplusplus
}
#endif
