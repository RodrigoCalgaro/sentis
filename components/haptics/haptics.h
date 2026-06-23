#pragma once

#include "esp_err.h"

// =============================================================================
// haptics.h — API pública del componente de vibración
//
// Los dos motores tipo moneda (coin vibration motors) se controlan mediante
// señales PWM generadas por el periférico LEDC del ESP32-P4.
// No requieren chip driver externo: la señal PWM enciende/apaga el motor
// directamente a través del transistor de conmutación de la placa.
//
// Patrones disponibles:
//   OFF          → ambos motores apagados
//   PULSE_LEFT   → motor izquierdo pulsa intermitentemente (peligro a la izquierda)
//   PULSE_RIGHT  → motor derecho pulsa intermitentemente  (peligro a la derecha)
//   PULSE_BOTH   → ambos motores pulsan juntos             (obstáculo frontal lejano)
//   ALERT_BOTH   → ambos motores vibran sin pausa          (obstáculo frontal inminente)
// =============================================================================

typedef enum {
    HAPTIC_PATTERN_OFF = 0,
    HAPTIC_PATTERN_PULSE_LEFT,
    HAPTIC_PATTERN_PULSE_RIGHT,
    HAPTIC_PATTERN_PULSE_BOTH,   // aviso suave: zona de precaución
    HAPTIC_PATTERN_ALERT_BOTH,   // alerta máxima: obstáculo muy cercano
} haptic_pattern_t;

// Inicializa los dos canales LEDC PWM y lanza la tarea de fondo que ejecuta
// los patrones. Debe llamarse una sola vez al inicio de app_main.
esp_err_t haptic_init(void);

// Cambia el patrón activo. Es seguro llamar desde cualquier tarea de FreeRTOS
// porque la escritura sobre un volatile uint8_t es atómica en Xtensa/RISC-V.
void haptic_set_pattern(haptic_pattern_t pattern);
