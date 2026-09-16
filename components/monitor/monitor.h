#pragma once

#include "esp_err.h"

// Instala el driver USB Serial/JTAG (puerto OTG nativo del ESP32-P4) con TX
// buffer de 64 KB y lanza monitor_task, que encoda cada frame de la cámara a
// JPEG y lo transmite por ESE puerto. Es un peripheral distinto del UART0 que
// usa la consola de logs (idf.py flash / idf.py monitor) — requiere que el
// secondary console de USB_SERIAL_JTAG esté deshabilitado en sdkconfig
// (CONFIG_ESP_CONSOLE_SECONDARY_NONE=y) para que este driver sea el único
// dueño del peripheral.
//
// Protocolo binario: magic(4) + size(4) + zone(1) + pad(1) + JPEG(N)
// Ver tools/monitor_viewer.py para el visualizador en el PC.
//
// monitor_viewer.py (puerto OTG) e idf.py monitor (puerto UART0) son COM
// ports distintos — pueden abrirse en simultáneo.
//
// Requiere CONFIG_MONITOR_ENABLED=y (menuconfig → SENTIS Monitor).
// Si el flag está desactivado, retorna ESP_OK inmediatamente sin hacer nada.
esp_err_t monitor_init(void);

// Publica el último texto STT reconocido para que el viewer lo muestre como overlay.
// text: cadena UTF-8 terminada en NUL (se trunca a 63 caracteres internamente).
// Thread-safe: puede llamarse desde cualquier tarea.
// No-op si CONFIG_MONITOR_ENABLED=n.
void monitor_set_stt_text(const char *text);
