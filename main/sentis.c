#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "haptics.h"
#include "lidar.h"
#include "vision.h"
#include "monitor.h"

// =============================================================================
// Umbrales de proximidad — ajustar estos dos valores para calibrar las distancias
// a las que el usuario recibe retroalimentación háptica.
//
//   PROXIMITY_WARN_MM   Distancia a partir de la cual comienza la vibración suave
//                       (motor izquierdo, derecho o ambos según posición del
//                       obstáculo detectada por la cámara). Indica que hay un
//                       obstáculo en la zona de precaución.
//                       Valor por defecto: 1500 mm (1,5 m)
//
//   PROXIMITY_ALERT_MM  Distancia a partir de la cual se activa la vibración
//                       continua a máxima intensidad en ambos motores. En esta
//                       zona el obstáculo es inminente y la prioridad es la
//                       seguridad: se ignora la posición lateral y se activan
//                       ambos motores para la reacción más rápida posible.
//                       Valor por defecto: 500 mm (50 cm)
//                       Debe ser menor que PROXIMITY_WARN_MM.
// =============================================================================
#define PROXIMITY_WARN_MM   1500
#define PROXIMITY_ALERT_MM   500

// Intervalo entre evaluaciones de proximidad en milisegundos.
// 50 ms → 20 evaluaciones por segundo, suficiente para obstáculos semi-dinámicos.
// La cámara corre en segundo plano a 15 fps (~67 ms por frame), por lo que en
// cada evaluación se lee el resultado del último frame analizado sin bloquear.
#define PROXIMITY_POLL_MS   50

// -----------------------------------------------------------------------------
// proximity_task — lee distancia del LiDAR, consulta posición de la cámara y
// selecciona el patrón háptico mediante la tabla de fusión.
//
// Tabla de decisión:
//
//   dist == 0 ó dist > WARN     → sin obstáculo relevante  → OFF
//
//   ALERT < dist ≤ WARN         → zona de precaución
//     cámara lista + LEFT       → PULSE_LEFT   (motor izquierdo pulsa)
//     cámara lista + RIGHT      → PULSE_RIGHT  (motor derecho pulsa)
//     cámara lista + CENTER     → PULSE_BOTH   (ambos pulsan, obstáculo frontal)
//     cámara lista + NONE       → PULSE_BOTH   (sin posición resuelta: fallback)
//     cámara no lista aún       → PULSE_BOTH   (fallback hasta primer frame)
//
//   dist ≤ ALERT                → zona de peligro
//     siempre                   → ALERT_BOTH   (ambos vibran, máxima prioridad)
//     (posición ignorada: a <50 cm la reacción es crítica y no hay margen)
//
// La función haptic_set_pattern es segura para llamar desde esta tarea porque
// la escritura sobre s_pattern es atómica (ver haptics.c).
// La lectura de vision_get_obstacle_side() también es atómica (volatile uint8_t).
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
            // Zona de precaución: el obstáculo se acerca pero hay margen.
            // Usar la posición lateral de la cámara para activar solo el motor
            // del lado correspondiente.
            if (!vision_is_ready()) {
                // La cámara aún no procesó su primer frame. Comportamiento de
                // arranque seguro: activar ambos motores hasta tener datos.
                pattern = HAPTIC_PATTERN_PULSE_BOTH;
            } else {
                switch (vision_get_obstacle_side()) {
                    case OBSTACLE_SIDE_LEFT:
                        pattern = HAPTIC_PATTERN_PULSE_LEFT;
                        break;
                    case OBSTACLE_SIDE_RIGHT:
                        pattern = HAPTIC_PATTERN_PULSE_RIGHT;
                        break;
                    case OBSTACLE_SIDE_CENTER:
                    case OBSTACLE_SIDE_NONE:
                    default:
                        // NONE: la heurística no resolvió una posición clara
                        // (sin bordes suficientes o escena muy uniforme).
                        // Fallback a ambos motores para no dejar al usuario
                        // sin retroalimentación.
                        pattern = HAPTIC_PATTERN_PULSE_BOTH;
                        break;
                }
            }

        } else {
            // Zona de peligro: obstáculo muy cercano, detención inmediata.
            // Seguridad primero: ambos motores a máxima intensidad,
            // independientemente de la posición lateral.
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
// Inicializa los tres subsistemas de hardware (motores, LiDAR y cámara) y lanza
// la tarea de proximidad que los coordina en tiempo real. Cada subsistema corre
// su propio proceso interno en FreeRTOS, por lo que app_main puede retornar una
// vez terminada la configuración.
//
// Orden de inicialización:
//   1. haptic_init  — LEDC PWM, sin dependencias externas
//   2. lidar_init   — UART1, sin dependencias externas
//   3. vision_init  — I2C + MIPI CSI-2; puede tardar más si el sensor realiza
//                     su secuencia de arranque. Un error aquí es no fatal: el
//                     sistema continúa operando con fallback LiDAR-only porque
//                     vision_is_ready() retorna false hasta el primer frame.
//   4. proximity_task — fusiona los datos de los tres anteriores
// -----------------------------------------------------------------------------
void app_main(void)
{
    haptic_init();
    lidar_init();

    // La cámara puede fallar si el cable CSI no está conectado o el sensor no
    // responde. En ese caso vision_is_ready() nunca retorna true y proximity_task
    // opera en modo fallback (solo LiDAR, ambos motores), preservando el
    // comportamiento original de las fases anteriores.
    vision_init();

    // Monitor WiFi solo para desarrollo (ver menuconfig → SENTIS Monitor).
    // No fatal si falla (WiFi no configurado o AP inalcanzable).
    monitor_init();

    xTaskCreate(proximity_task, "proximity", 2048, NULL, 5, NULL);
}
