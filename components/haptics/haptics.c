#include "haptics.h"
#include "board_config.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -----------------------------------------------------------------------------
// Configuración del periférico LEDC (LED Control / PWM)
// Se usa el timer 0 en modo baja velocidad, suficiente para motores DC.
// Frecuencia 5 kHz: por encima del umbral auditivo humano (~20 Hz–20 kHz), por
// lo que el tono portador no resulta molesto ni audible para el usuario.
// Resolución 10 bits → ciclo de trabajo en rango 0–1023.
// -----------------------------------------------------------------------------
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_MODE        LEDC_LOW_SPEED_MODE
#define LEDC_FREQ_HZ     5000
#define LEDC_RESOLUTION  LEDC_TIMER_10_BIT

#define DUTY_OFF  0
#define DUTY_MAX  ((1 << 10) - 1)   // 1023 = 100 % de intensidad

// Duración de cada fase del pulso en los patrones intermitentes.
// 200 ms ON + 200 ms OFF = frecuencia de pulso percibida de ~2,5 Hz.
#define PULSE_ON_MS   200
#define PULSE_OFF_MS  200

// Variable global del patrón activo. La lee haptic_task y la escribe
// haptic_set_pattern (puede llamarse desde otra tarea, de ahí el volatile).
static volatile haptic_pattern_t s_pattern = HAPTIC_PATTERN_OFF;

// -----------------------------------------------------------------------------
// set_duty — aplica un ciclo de trabajo a un canal LEDC.
// El periférico LEDC requiere dos pasos: primero se carga el nuevo duty con
// ledc_set_duty y luego se confirma con ledc_update_duty para que surta efecto.
// -----------------------------------------------------------------------------
static void set_duty(ledc_channel_t ch, uint32_t duty)
{
    ledc_set_duty(LEDC_MODE, ch, duty);
    ledc_update_duty(LEDC_MODE, ch);
}

// -----------------------------------------------------------------------------
// haptic_task — máquina de estados que ejecuta el patrón activo en un bucle.
//
// Corre en su propia tarea de FreeRTOS para que los vTaskDelay de los patrones
// intermitentes no bloqueen la lógica del resto del sistema. La tarea lee
// s_pattern en cada iteración, lo que permite cambiar el patrón al instante
// desde cualquier otra tarea sin necesidad de sincronización explícita.
// -----------------------------------------------------------------------------
static void haptic_task(void *arg)
{
    while (1) {
        switch (s_pattern) {

            // Motor izquierdo enciende sólo → señal de peligro a la izquierda.
            case HAPTIC_PATTERN_PULSE_LEFT:
                set_duty(LEDC_CHANNEL_0, DUTY_MAX);
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_ON_MS));
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_OFF_MS));
                break;

            // Motor derecho enciende sólo → señal de peligro a la derecha.
            case HAPTIC_PATTERN_PULSE_RIGHT:
                set_duty(LEDC_CHANNEL_1, DUTY_MAX);
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_ON_MS));
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_OFF_MS));
                break;

            // Ambos motores pulsan juntos → aviso de obstáculo frontal en zona
            // de precaución. El ritmo intermitente indica "atención, aún hay margen".
            case HAPTIC_PATTERN_PULSE_BOTH:
                set_duty(LEDC_CHANNEL_0, DUTY_MAX);
                set_duty(LEDC_CHANNEL_1, DUTY_MAX);
                vTaskDelay(pdMS_TO_TICKS(PULSE_ON_MS));
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(PULSE_OFF_MS));
                break;

            // Ambos motores a máxima intensidad de forma continua → obstáculo
            // inminente, el usuario debe detenerse de inmediato.
            case HAPTIC_PATTERN_ALERT_BOTH:
                set_duty(LEDC_CHANNEL_0, DUTY_MAX);
                set_duty(LEDC_CHANNEL_1, DUTY_MAX);
                vTaskDelay(pdMS_TO_TICKS(50));   // delay mínimo para ceder CPU
                break;

            // Patrón por defecto o apagado explícito: ambos motores detenidos.
            case HAPTIC_PATTERN_OFF:
            default:
                set_duty(LEDC_CHANNEL_0, DUTY_OFF);
                set_duty(LEDC_CHANNEL_1, DUTY_OFF);
                vTaskDelay(pdMS_TO_TICKS(50));
                break;
        }
    }
}

// -----------------------------------------------------------------------------
// haptic_init — configura el periférico LEDC y lanza la tarea de patrones.
//
// Se configura un único timer compartido por los dos canales (uno por motor).
// Cada canal se asigna al GPIO correspondiente definido en board_config.h.
// La tarea se crea con prioridad 5, igual que la de proximidad, para que
// los patrones se actualicen con latencia mínima.
// -----------------------------------------------------------------------------
esp_err_t haptic_init(void)
{
    // Configurar el timer PWM compartido por ambos canales.
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    // Canal 0 → motor izquierdo (GPIO definido en board_config.h).
    ledc_channel_config_t ch_left = {
        .channel    = LEDC_CHANNEL_0,
        .duty       = 0,
        .hpoint     = 0,
        .gpio_num   = BOARD_HAPTIC_LEFT_GPIO,
        .speed_mode = LEDC_MODE,
        .timer_sel  = LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_left));

    // Canal 1 → motor derecho (GPIO definido en board_config.h).
    ledc_channel_config_t ch_right = {
        .channel    = LEDC_CHANNEL_1,
        .duty       = 0,
        .hpoint     = 0,
        .gpio_num   = BOARD_HAPTIC_RIGHT_GPIO,
        .speed_mode = LEDC_MODE,
        .timer_sel  = LEDC_TIMER,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch_right));

    // Lanzar la tarea que ejecuta los patrones de vibración de forma continua.
    xTaskCreate(haptic_task, "haptic", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// haptic_set_pattern — cambia el patrón activo desde cualquier contexto.
// La escritura es atómica en la arquitectura del ESP32-P4 (RISC-V/Xtensa),
// por lo que no se requiere mutex para este acceso de un solo escritor.
// -----------------------------------------------------------------------------
void haptic_set_pattern(haptic_pattern_t pattern)
{
    s_pattern = pattern;
}
