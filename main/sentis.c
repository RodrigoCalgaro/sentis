#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "haptics.h"
#include "lidar.h"
#include "vision.h"
#include "monitor.h"
#include "storage.h"
#include "audio.h"
#include "mic.h"
#include "stt.h"
#include "tts.h"

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
// on_stt_result — callback invocada por el componente stt cuando se reconoce
// un comando de voz.
//
// Responsabilidades:
//   1. Log en consola (siempre visible en idf.py monitor).
//   2. Publicar en el monitor viewer para overlay visual (si está habilitado).
//
// Nota: esta función se llama desde mic_task (prioridad 6), directamente
// dentro del loop de alimentación de ESP-SR — no bloquear aquí.
// -----------------------------------------------------------------------------
static void on_stt_result(const stt_result_t *result)
{
    // El log aparece en el monitor serie aunque el viewer gráfico esté apagado.
    // Formato consistente con el resto de los logs del proyecto.
    // Nivel INFO para que sea visible en builds de producción.
    ESP_LOGI("stt", "COMANDO: [%d] \"%s\"", result->command_id, result->text);

    // Publicar al viewer gráfico (no-op si CONFIG_MONITOR_ENABLED=n).
    monitor_set_stt_text(result->text);
}

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
// Orden de inicialización:
//   1. haptic_init   — LEDC PWM, sin dependencias externas
//   2. lidar_init    — UART1, sin dependencias externas
//   3. storage_init  — SDMMC 4-bit → FAT VFS en /sdcard (Fase 2)
//   4. audio_init    — ES8311 + I2S0 full-duplex + NS4150B (Fase 2 + Fase 4)
//                      Abre TX (playback) y RX (micrófono) en el mismo I2S0.
//   5. tts_init      — carga voz eSpeak-NG desde SD (Fase 6A)
//                      Reproduce "Sentis Encendido" como confirmación de arranque.
//   6. stt_init      — carga modelos ESP-SR desde SD (Fase 4)
//   7. mic_init      — tarea de captura: ES8311 ADC → chunks mono → stt_feed()
//   8. vision_init   — I2C + MIPI CSI-2 (Fase 5)
//   9. monitor_init  — transmisión de frames para desarrollo (Fase 5)
//  10. proximity_task — fusiona LiDAR + visión + háptica
//
// Nota Fase 4: stt_init() necesita que los modelos estén en la partición "model"
// (partitions.csv). Tras un build limpio ejecutar:
//   del sdkconfig && idf.py set-target esp32p4 && idf.py flash
// -----------------------------------------------------------------------------
void app_main(void)
{
    haptic_init();
    lidar_init();

    // ---- Fase 2: almacenamiento y audio ----
    storage_init();   // no fatal — logs error si no hay tarjeta
    audio_init();     // ES8311 + I2S0 full-duplex (TX playback + RX mic)

    if (storage_is_mounted()) {
        audio_play_wav("/sdcard/alert.wav");
    }

    // ---- Fase 6A: TTS en español (eSpeak-NG desde SD) ----
    // tts_init carga los datos de voz desde /sdcard/espeak-ng-data/.
    // No fatal: si los archivos no están en la SD, se loguea el error
    // y el sistema sigue operando sin TTS.
    if (storage_is_mounted()) {
        if (tts_init("/sdcard/espeak-ng-data") == ESP_OK) {
            tts_speak("Sentis Encendido");
        }
    }

    // ---- Fase 4: reconocimiento de voz (STT local via ESP-SR) ----
    // stt_init carga los modelos MultiNet desde la partición "model".
    // No fatal: si la partición no existe o está vacía, se loguea el error
    // y el sistema sigue operando sin STT.
    if (stt_init(on_stt_result) == ESP_OK) {
        // mic_init arranca la tarea de captura: I2S0 RX → downmix → stt_feed()
        // La tarea alimenta ESP-SR sincrónicamente — sin latencia adicional.
        mic_init(stt_feed);
    }

    // ---- Fase 5: cámara ----
    vision_init();

    // Monitor visual solo para desarrollo (menuconfig → SENTIS Monitor).
    // monitor_init();

    xTaskCreate(proximity_task, "proximity", 2048, NULL, 5, NULL);
}
