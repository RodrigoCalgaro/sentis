#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// wifi — SoftAP del ESP32-P4 para la app Android companera.
//
// El ESP32-P4 no tiene radio propia: el trafico WiFi corre sobre el
// co-procesador ESP32-C6 a bordo, conectado por SDIO, via el componente
// espressif/esp_hosted (transporte transparente para la aplicacion — esta
// capa solo usa esp_wifi.h estandar, sin API propia de esp_hosted mas alla
// del Kconfig de transporte).
//
// wifi_init() levanta un SoftAP (SSID/password/canal por Kconfig, ver
// Kconfig.projbuild) para que la app Android se conecte como cliente fijo
// en 192.168.4.1. No fatal para el resto del sistema si falla: logea el
// error y retorna (mismo criterio que storage_init()/tts_init()/
// stt_init()/ocr_init() en main/sentis.c).
//
// Dueno de nvs_flash_init()/esp_netif_init()/esp_event_loop_create_default():
// nada mas en este proyecto los llama todavia — wifi_init() los inicializa
// el mismo, una sola vez, antes de tocar esp_wifi.
//
// MILESTONE 0 (bring-up aislado, ver plan de migracion OCR+STT a app
// companion): este componente es la primera pieza a validar en hardware
// real. Confirmar en el log que el SoftAP queda arriba y que un celular
// puede verlo/conectarse ANTES de construir el protocolo `link` encima.
//
// Riesgo de hardware CONFIRMADO EN HARDWARE (2026-09-16, ver
// sdkconfig.defaults para el detalle completo): el pin de reset del C6
// (GPIO54) es el MISMO que board_config.h usa para el XCLK de la camara
// OV5647. Se intento deshabilitarlo (-1) pero esp_hosted lo rechaza con un
// assert incondicional al arrancar — hace falta un pin real. Se usa GPIO54
// tal cual, confiando en que wifi_init() (pulso de reset, una sola vez)
// corre antes que vision_init() en main/sentis.c. Si la camara falla
// despues de este cambio, este es el sospechoso numero uno.
// =============================================================================

// Levanta NVS + netif + el SoftAP. Idempotente-safe de llamar una vez en
// app_main(). No bloqueante mas alla de la negociacion normal de
// esp_wifi_start().
esp_err_t wifi_init(void);

// true si hay al menos un cliente (celular) conectado al SoftAP en este
// momento.
bool wifi_is_client_connected(void);

#ifdef __cplusplus
}
#endif
