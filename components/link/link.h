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
//   LINK_MSG_SETTINGS_SET   pedido de cambio de un parametro ajustable
//                        (volumen o umbrales de proximidad) — ver
//                        link_setting_id_t. Aplicado por components/settings
//                        via los callbacks que se le pasan a link_init().
// Mensajes ESP32 -> celular (cont.):
//   LINK_MSG_SETTINGS_STATE  estado actual de los tres parametros
//                        ajustables. Se manda solo cuando cambia algo
//                        relevante para la UI de la app: al conectarse el
//                        celular (para poblar los sliders con el valor real
//                        en vez de un default hardcodeado) y despues de
//                        aplicar cada LINK_MSG_SETTINGS_SET (con el valor ya
//                        clampeado/validado, que puede no ser el pedido).
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

// IDs de parametro para LINK_MSG_SETTINGS_SET — deben coincidir con las
// constantes SETTING_* de LinkClient.kt (mismo criterio que los tipos de
// mensaje LINK_MSG_*: sin header compartido entre C y Kotlin, los valores
// numericos son el contrato).
typedef enum {
    LINK_SETTING_VOLUME             = 1,  // 0-100 (%)
    LINK_SETTING_PROXIMITY_WARN_MM  = 2,  // mm
    LINK_SETTING_PROXIMITY_ALERT_MM = 3,  // mm
} link_setting_id_t;

// Estado completo de los tres parametros ajustables, para LINK_MSG_SETTINGS_STATE.
typedef struct {
    int32_t volume_pct;
    int32_t proximity_warn_mm;
    int32_t proximity_alert_mm;
} link_settings_state_t;

// Callback invocada cuando llega un LINK_MSG_SETTINGS_SET — quien la
// implementa (ver on_link_settings_set en main/sentis.c) valida, persiste y
// aplica el cambio (components/settings). Se llama desde la tarea de
// recepcion de link — no bloquear aca (mismo criterio que link_command_cb_t).
typedef void (*link_settings_set_cb_t)(link_setting_id_t param_id, int32_t value);

// Callback para leer el estado actual de los tres parametros, usada por link
// para armar un LINK_MSG_SETTINGS_STATE (al conectar el celular y despues de
// cada SET aplicado). No deberia fallar ni bloquear — es una lectura simple
// de components/settings.
typedef link_settings_state_t (*link_settings_get_cb_t)(void);

// Levanta el servidor TCP (tareas de aceptar conexion, recibir mensajes y
// enviar audio encolado). Requiere wifi_init() ya corrido (el SoftAP debe
// estar arriba). command_cb puede ser NULL si todavia no hay nada que reciba
// comandos (se loguea el comando y se descarta). settings_set_cb/settings_get_cb
// pueden ser NULL si todavia no hay ajustes que exponer (se loguea y se
// descarta el SET; no se manda STATE).
esp_err_t link_init(link_command_cb_t command_cb,
                     link_settings_set_cb_t settings_set_cb,
                     link_settings_get_cb_t settings_get_cb);

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
