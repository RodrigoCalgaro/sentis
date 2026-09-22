#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// settings — parámetros ajustables en runtime desde la app Android companion
// (volumen del parlante, umbrales de distancia del LiDAR que disparan la
// vibración), persistidos en NVS (namespace "settings") para sobrevivir un
// reinicio.
//
// Dueño único de estos tres valores: main/sentis.c (proximity_task) y
// components/audio ya no usan constantes fijas para ellos, sino los getters
// de acá. El componente components/link invoca los setters (a través de los
// callbacks link_settings_set_cb_t / link_settings_get_cb_t que main/sentis.c
// le pasa a link_init) cuando llega un LINK_MSG_SETTINGS_SET desde la app.
//
// Invariante de seguridad (igual criterio que documentaba el comentario de
// PROXIMITY_ALERT_MM en main/sentis.c): proximity_alert_mm siempre debe ser
// menor que proximity_warn_mm, con un margen mínimo. Los setters rechazan
// (ESP_ERR_INVALID_ARG, sin persistir) cualquier valor que rompa esto — la
// app se entera porque el LINK_MSG_SETTINGS_STATE que le sigue no va a
// reflejar el cambio pedido.
// =============================================================================

#define SETTINGS_DEFAULT_VOLUME_PERCENT       70
#define SETTINGS_DEFAULT_PROXIMITY_WARN_MM   1500
#define SETTINGS_DEFAULT_PROXIMITY_ALERT_MM   500

#define SETTINGS_PROXIMITY_MIN_GAP_MM  50
#define SETTINGS_PROXIMITY_WARN_MIN_MM   300
#define SETTINGS_PROXIMITY_WARN_MAX_MM  4000
#define SETTINGS_PROXIMITY_ALERT_MIN_MM  100
#define SETTINGS_PROXIMITY_ALERT_MAX_MM 2000

// Carga los tres valores desde NVS (namespace "settings"), o los defaults de
// arriba si todavía no se guardó nada (primer boot / NVS borrada). No toca el
// codec de audio — llamar a audio_set_volume(settings_get_volume_percent())
// una vez que audio_init() ya corrió (ver orden en main/sentis.c). No fatal:
// si NVS falla, queda todo en los defaults y los setters simplemente no
// van a persistir (se loguea, se sigue con el valor en memoria).
esp_err_t settings_init(void);

int      settings_get_volume_percent(void);
uint16_t settings_get_proximity_warn_mm(void);
uint16_t settings_get_proximity_alert_mm(void);

// Clampea a [0,100], persiste en NVS, aplica de inmediato con
// audio_set_volume(), y dispara un tono de prueba corto (audio_test_tone())
// en una tarea aparte para que se escuche el volumen nuevo sin bloquear a
// quien llama (pensado para el receive loop de components/link).
esp_err_t settings_set_volume_percent(int vol_percent);

// Clampea a [SETTINGS_PROXIMITY_WARN_MIN_MM, SETTINGS_PROXIMITY_WARN_MAX_MM]
// y persiste en NVS. ESP_ERR_INVALID_ARG (sin persistir) si el resultado
// queda a menos de SETTINGS_PROXIMITY_MIN_GAP_MM del alert_mm actual.
esp_err_t settings_set_proximity_warn_mm(int warn_mm);

// Como settings_set_proximity_warn_mm(), para el umbral de alerta —
// clampea a [SETTINGS_PROXIMITY_ALERT_MIN_MM, SETTINGS_PROXIMITY_ALERT_MAX_MM].
esp_err_t settings_set_proximity_alert_mm(int alert_mm);

#ifdef __cplusplus
}
#endif
