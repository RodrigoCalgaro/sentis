#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

// Dimensiones del frame capturado — expuestas para el componente monitor.
#define VISION_FRAME_W   800
#define VISION_FRAME_H   640
#define VISION_FRAME_SZ  (VISION_FRAME_W * VISION_FRAME_H)

// =============================================================================
// vision.h — API pública del componente de visión
//
// Este componente integra la cámara OV5647 (MIPI CSI-2) con una heurística de
// detección de posición por análisis de actividad de bordes. Su único propósito
// en la Fase 5 es responder a la pregunta "¿de qué lado está el obstáculo?"
// para que proximity_task pueda seleccionar el motor háptico correcto.
//
// Algoritmo:
//   Se captura un frame en baja resolución (QVGA, escala de grises equivalente).
//   El frame se divide en tres franjas verticales iguales (izquierda / centro /
//   derecha). En cada franja se calcula el promedio de diferencias absolutas
//   entre píxeles horizontales adyacentes — una medida de "riqueza de bordes".
//   La franja con mayor actividad por encima del umbral mínimo determina el lado.
//   Si ninguna zona supera el umbral, se retorna OBSTACLE_SIDE_NONE.
//
// Flujo de integración:
//   1. Llamar vision_init() una sola vez desde app_main (después de haptic_init
//      y lidar_init).
//   2. En proximity_task, antes de seleccionar el patrón háptico, verificar
//      vision_is_ready() y luego consultar vision_get_obstacle_side().
//   3. Si vision_is_ready() retorna false, usar el comportamiento de fallback
//      (PULSE_BOTH) hasta que la cámara capture su primer frame válido.
// =============================================================================

typedef enum {
    OBSTACLE_SIDE_NONE = 0,  // sin actividad de bordes detectable: fallback a PULSE_BOTH
    OBSTACLE_SIDE_LEFT,      // mayor actividad en el tercio izquierdo del frame
    OBSTACLE_SIDE_CENTER,    // mayor actividad en el tercio central del frame
    OBSTACLE_SIDE_RIGHT,     // mayor actividad en el tercio derecho del frame
} obstacle_side_t;

// Inicializa el bus I2C, detecta el sensor OV5647, configura el pipeline MIPI
// CSI-2 y lanza la tarea de captura y análisis en segundo plano.
// Debe llamarse una sola vez desde app_main.
esp_err_t vision_init(void);

// Retorna el lado del obstáculo calculado en el último frame analizado.
// Es seguro llamar desde cualquier tarea: la lectura de s_side es atómica.
// Retorna OBSTACLE_SIDE_NONE si la cámara aún no capturó ningún frame.
obstacle_side_t vision_get_obstacle_side(void);

// Retorna true una vez que vision_task procesó al menos un frame correctamente.
// Usar esto como guarda en proximity_task para evitar actuar sobre datos vacíos.
bool vision_is_ready(void);

// Copia el último frame capturado en dst. dst debe tener capacidad para al
// menos VISION_FRAME_SZ bytes y estar en memoria accesible por DMA.
// Bloquea hasta 50 ms esperando el mutex. Retorna false si la cámara aún no
// capturó ningún frame o si dst/len son inválidos.
bool vision_copy_display_frame(uint8_t *dst, size_t len);
