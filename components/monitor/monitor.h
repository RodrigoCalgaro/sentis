#pragma once

#include "esp_err.h"

// Instala el driver USB Serial/JTAG con TX buffer de 64 KB y lanza monitor_task,
// que encoda cada frame de la cámara a JPEG y lo transmite por el mismo puerto
// USB que se usa para flashear y ver el console (idf.py monitor).
//
// Protocolo binario: magic(4) + size(4) + zone(1) + pad(1) + JPEG(N)
// Ver tools/monitor_viewer.py para el visualizador en el PC.
//
// NOTA: monitor_viewer.py y idf.py monitor no pueden estar abiertos al mismo
// tiempo (mismo COM port). Usar uno u otro según la tarea.
//
// Requiere CONFIG_MONITOR_ENABLED=y (menuconfig → SENTIS Monitor).
// Si el flag está desactivado, retorna ESP_OK inmediatamente sin hacer nada.
esp_err_t monitor_init(void);
