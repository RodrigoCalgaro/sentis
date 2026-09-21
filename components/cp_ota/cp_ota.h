#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// cp_ota — sube el firmware correcto al co-procesador ESP32-C6 via OTA sobre
// SDIO (esp_hosted), leyendo el binario desde la particion "slave_fw" del
// propio ESP32-P4. Ver coprocessor/README.md para como compilar y copiar ese
// binario a la particion.
//
// Motivo: el C6 de esta placa vino de fabrica con un firmware generico que
// reporta version "0.0.0" — no es un release real de esp_hosted (confirmado
// en hardware, 2026-09-18: "major version mismatch — OTA coprocessor from
// host" en el log de boot, y sospechoso de varios problemas de estabilidad
// encontrados esa sesion).
//
// cp_ota_check_and_update() requiere que wifi_init() ya haya levantado el
// link con el C6. No fatal: si la particion esta vacia (placa recien
// clonada, sin provisionar) o el OTA falla, logea el error y retorna — el
// resto del sistema sigue funcionando con el firmware que ya tenga el C6.
// =============================================================================

esp_err_t cp_ota_check_and_update(void);

#ifdef __cplusplus
}
#endif
