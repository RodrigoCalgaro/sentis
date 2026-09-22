#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// link — protocolo de aplicacion hacia la app Android companion, sobre el
// SoftAP que levanta components/wifi (192.168.4.1). El ESP32 es el servidor
// TCP (puerto LINK_TCP_PORT), la app se conecta como cliente unico.
//
// Framing: magic(4) + tipo(1) + pad(3) + tamano_payload(4) + payload.
// Mismo espiritu que components/monitor/monitor.c (magic+tipo+tamano), pero
// en un socket TCP en vez de USB serial, y con mensajes en ambas direcciones.
//
// Mensajes ESP32 -> celular:
//   LINK_MSG_AUDIO       PCM mono 16kHz 16-bit (mismo chunk que produce mic)
//   LINK_MSG_OCR_REQUEST JPEG de un frame (pedido de lectura OCR)
//   LINK_MSG_COLOR_REQUEST JPEG de un frame (pedido de deteccion de color,
//                        ver components/ocr — mismo capturador que OCR, un
//                        solo pedido/respuesta en vez de un loop)
// Mensajes celular -> ESP32:
//   LINK_MSG_COMMAND     comando de voz reconocido (Vosk, en la app)
//   LINK_MSG_OCR_RESULT  texto reconocido (ML Kit, en la app) para el ultimo
//                        LINK_MSG_OCR_REQUEST pendiente
//   LINK_MSG_COLOR_RESULT nombre del color dominante (analisis HSV on-device,
//                        en la app) para el ultimo LINK_MSG_COLOR_REQUEST
//                        pendiente
//   LINK_MSG_TTS_AUDIO   PCM mono 16kHz 16-bit sintetizado por el TTS nativo
//                        de Android (mejor cadencia que eSpeak-NG) — se
//                        reproduce por el parlante del propio ESP32, no el
//                        del telefono (decision del usuario 2026-09-21).
//                        Chunks de hasta LINK_TTS_AUDIO_MAX_SAMPLES, uno o
//                        mas por locucion.
//
// link_init() no es fatal para el resto del sistema si falla (mismo criterio
// que storage_init()/tts_init()/ocr_init()/wifi_init()).
//
// Fase 2 (ver sentis-stability-integration-plan.md): conectado a mic/ocr
// reales — main/sentis.c llama mic_init(link_send_audio) directo, y
// components/ocr/ocr.cpp llama link_request_ocr_text() en vez de correr
// inferencia on-device. La tarea de autotest temporal (audio/JPEG
// sinteticos, validada contra tools/link_test_client.py) ya se sacó.
//
// Fase 3.1: el texto de OCR ya no se locuta con tts_speak() en el ESP32
// (ver components/ocr/ocr.cpp) — la app sintetiza con el TTS de Android y
// manda el PCM resultante de vuelta por LINK_MSG_TTS_AUDIO, para que salga
// por el parlante de SENTIS. El componente tts del ESP32 (eSpeak-NG) sigue
// activo para mensajes que no dependen del celular (ej. "Sentis Encendido").
// =============================================================================

#define LINK_TCP_PORT          3333
#define LINK_COMMAND_TEXT_MAX  64
#define LINK_OCR_TEXT_MAX      256
#define LINK_COLOR_TEXT_MAX    32

typedef struct {
    int  command_id;
    char text[LINK_COMMAND_TEXT_MAX];
} link_command_t;

// Callback invocada cada vez que llega un comando de voz reconocido desde
// la app. Se llama desde la tarea de recepcion de link — no bloquear aca
// (mismo criterio que on_stt_result() en main/sentis.c).
typedef void (*link_command_cb_t)(const link_command_t *cmd);

// Levanta el servidor TCP (tareas de aceptar conexion, recibir mensajes y
// enviar audio encolado). Requiere wifi_init() ya corrido (el SoftAP debe
// estar arriba). cb puede ser NULL si todavia no hay nada que reciba
// comandos (se loguea el comando y se descarta).
esp_err_t link_init(link_command_cb_t cb);

// true si la app esta conectada en este momento.
bool link_is_client_connected(void);

// Encola un chunk de audio para enviar al celular. No bloqueante — si la
// cola esta llena (celular no conectado o con congestion) se descarta el
// chunk mas viejo. Mismo perfil que mic_data_cb_t (components/mic/mic.h):
// pasar directo como callback a mic_init() cuando se haga el wiring real.
void link_send_audio(const int16_t *samples, size_t count);

// Manda un frame JPEG como pedido de OCR y bloquea hasta recibir el texto
// reconocido (LINK_MSG_OCR_RESULT) o timeout_ticks. out_text queda con un
// string vacio si no hubo texto reconocido. Llamada pensada para usarse
// desde ocr_task exactamente como hoy usa s_det->run()/s_rec->run().
esp_err_t link_request_ocr_text(const uint8_t *jpeg, size_t jpeg_len,
                                 char *out_text, size_t out_text_max,
                                 TickType_t timeout_ticks);

// Como link_request_ocr_text(), pero para el pedido de deteccion de color
// (ver components/ocr::ocr_detect_color()): manda un frame JPEG como
// LINK_MSG_COLOR_REQUEST y bloquea hasta recibir el nombre del color
// (LINK_MSG_COLOR_RESULT) o timeout_ticks. out_text queda con un string
// vacio si no hubo respuesta.
esp_err_t link_request_color(const uint8_t *jpeg, size_t jpeg_len,
                              char *out_text, size_t out_text_max,
                              TickType_t timeout_ticks);

#ifdef __cplusplus
}
#endif
