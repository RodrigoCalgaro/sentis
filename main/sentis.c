#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "lidar.h"

// =============================================================================
// Umbrales de proximidad — ajustar estos dos valores para calibrar las distancias
// a las que el usuario recibe retroalimentación háptica.
//
//   PROXIMITY_WARN_MM   Distancia a partir de la cual comienza la vibración suave
//                       (ambos motores pulsan intermitentemente). Indica que hay
//                       un obstáculo en la zona de precaución.
//                       Valor por defecto: 1500 mm (1,5 m)
//
//   PROXIMITY_ALERT_MM  Distancia a partir de la cual se activa la vibración
//                       continua a máxima intensidad. Indica obstáculo inminente:
//                       el usuario debe detenerse de inmediato.
//                       Valor por defecto: 500 mm (50 cm)
//                       Debe ser menor que PROXIMITY_WARN_MM.
// =============================================================================
#define PROXIMITY_WARN_MM   1500
#define PROXIMITY_ALERT_MM   500

// Intervalo entre lecturas del sensor en la tarea de proximidad.
// 50 ms → 20 evaluaciones por segundo, suficiente para detección de obstáculos
// estáticos y semi-dinámicos (personas caminando).
#define PROXIMITY_POLL_MS   50

// -----------------------------------------------------------------------------
// proximity_task — lee la distancia del LiDAR y actualiza el patrón háptico.
//
// En cada ciclo consulta la última medición disponible (provista por lidar_task
// en segundo plano) y determina en qué zona se encuentra el obstáculo:
//
//   dist == 0 ó dist > WARN   →  sin obstáculo relevante → motores apagados
//   ALERT < dist ≤ WARN       →  zona de precaución      → pulsos suaves en ambos motores
//   dist ≤ ALERT              →  zona de peligro         →  vibración continua máxima
//
// La función haptic_set_pattern es segura para llamar desde esta tarea porque
// la escritura sobre s_pattern es atómica (ver haptics.c).
// -----------------------------------------------------------------------------
static void proximity_task(void *arg)
{
    while (1) {
        uint16_t dist = lidar_get_distance_mm();

        haptic_pattern_t pattern;

        if (dist == 0 || dist > PROXIMITY_WARN_MM) {
            // Sin datos todavía, o el obstáculo está fuera del rango de interés.
            pattern = HAPTIC_PATTERN_OFF;
        } else if (dist > PROXIMITY_ALERT_MM) {
            // Zona de precaución: el obstáculo se acerca pero hay margen de reacción.
            pattern = HAPTIC_PATTERN_PULSE_BOTH;
        } else {
            // Zona de peligro: obstáculo muy cercano, detención inmediata.
            pattern = HAPTIC_PATTERN_ALERT_BOTH;
        }

        haptic_set_pattern(pattern);

        // Ceder CPU al resto del sistema durante el intervalo de muestreo.
        vTaskDelay(pdMS_TO_TICKS(PROXIMITY_POLL_MS));
    }
}

// -----------------------------------------------------------------------------
// app_main — punto de entrada del firmware SENTIS.
//
// Inicializa los dos subsistemas de hardware (motores y LiDAR) y lanza la
// tarea de proximidad que los mantiene coordinados en tiempo real.
// Cada subsistema corre su propio proceso interno en FreeRTOS, por lo que
// app_main puede retornar una vez terminada la configuración.
// -----------------------------------------------------------------------------
void app_main(void)
{
    haptic_init();   // configura PWM y lanza haptic_task
    lidar_init();    // configura UART y lanza lidar_task
    xTaskCreate(proximity_task, "proximity", 2048, NULL, 5, NULL);
}
