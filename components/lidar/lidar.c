#include "lidar.h"
#include "board_config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

#define TAG "lidar"

// =============================================================================
// SP10M01 — sensor LiDAR DToF de punto único, UART full-duplex 3.3 V, 460800 baud.
//
// Conexión física (colores de cable de esta unidad):
//   Verde   = TX del sensor → GPIO4 (RX del ESP32)
//   Amarillo= RX del sensor → GPIO5 (TX del ESP32)
//   Negro   = GND
//   Rojo    = 3V3
//
// Protocolo (ingeniería inversa + calibración, 2026-06-16):
//   El sensor transmite tramas de forma autónoma sin necesidad de comando de inicio.
//   Cada trama tiene 4 bytes con la siguiente estructura:
//
//     [0x5C] [dist_low] [dist_high] [checksum]
//
//     0x5C       = byte de cabecera fijo que identifica el inicio de trama
//     dist_low   = byte bajo de la distancia en mm (little-endian)
//     dist_high  = byte alto de la distancia en mm (little-endian)
//     checksum   = 0xFF - dist_low - dist_high  (verificación de integridad)
//
//   Distancia: distance_mm = dist_low | (dist_high << 8)
//   Calibración confirmada: objeto a 24 cm → dato recibido = 0xF4 → 244 mm ✓
// =============================================================================

#define SP10_HEADER    0x5C   // byte de cabecera que identifica el inicio de trama
#define SP10_FRAME_LEN 4      // longitud fija de cada trama en bytes
#define SP10_BAUD      460800 // velocidad de comunicación del sensor

#define UART_BUF_SIZE  512    // tamaño del buffer interno del driver UART de ESP-IDF

// Última distancia válida recibida, en milímetros.
// Volatile porque la escribe lidar_task y la lee la tarea de proximidad.
static volatile uint16_t s_distance_mm = 0;

// Buffer auxiliar para la lectura inicial de validación.
static uint8_t s_rx[256];

// -----------------------------------------------------------------------------
// find_sp10_frame — busca la primera trama SP10 válida en un buffer.
//
// Recorre el buffer buscando el byte de cabecera 0x5C y verifica que los tres
// bytes siguientes cumplan la condición de checksum: dist_low + dist_high +
// checksum == 0xFF (módulo 256). Retorna el índice del inicio de la trama
// encontrada, o -1 si no hay ninguna trama válida en el rango dado.
//
// Se usa al arranque para confirmar que el sensor está enviando datos correctos
// antes de entrar en el bucle de lectura byte a byte.
// -----------------------------------------------------------------------------
static int find_sp10_frame(const uint8_t *buf, int n)
{
    for (int i = 0; i + SP10_FRAME_LEN <= n; i++) {
        if (buf[i] == SP10_HEADER &&
            (uint8_t)(buf[i+1] + buf[i+2] + buf[i+3]) == 0xFF)
            return i;
    }
    return -1;
}

// -----------------------------------------------------------------------------
// sp10_distance_mm — decodifica la distancia a partir de una trama completa.
//
// Los bytes 1 y 2 de la trama contienen la distancia en formato little-endian:
// el byte 1 es el byte menos significativo y el byte 2 el más significativo.
// Se reconstruye el valor de 16 bits desplazando el byte alto 8 posiciones.
// -----------------------------------------------------------------------------
static uint16_t sp10_distance_mm(const uint8_t *frame)
{
    return (uint16_t)frame[1] | ((uint16_t)frame[2] << 8);
}

// -----------------------------------------------------------------------------
// lidar_task — tarea de fondo que recibe y decodifica el stream del sensor.
//
// Flujo de ejecución:
//   1. Espera 500 ms para que el sensor termine su arranque interno.
//   2. Lee un bloque inicial y busca al menos una trama válida para confirmar
//      que el cable está bien conectado y el sensor responde. Si no hay datos
//      o no hay trama válida, la tarea se destruye a sí misma y reporta el error.
//   3. En el bucle principal, sincroniza la lectura al byte de cabecera 0x5C:
//      descarta cualquier byte que no sea la cabecera, y cuando la encuentra
//      lee los 3 bytes restantes de la trama. Si el checksum es correcto,
//      actualiza s_distance_mm con la nueva medición.
//
// Este enfoque de sincronización por cabecera tolera que el primer byte leído
// al arrancar pueda estar en medio de una trama ya iniciada.
// -----------------------------------------------------------------------------
static void lidar_task(void *arg)
{
    // Pausa para que el sensor complete su rutina de inicialización interna.
    vTaskDelay(pdMS_TO_TICKS(500));

    // --- Validación inicial: confirmar que el sensor envía tramas correctas ---
    uart_flush_input(BOARD_LIDAR_UART_NUM);
    int n = uart_read_bytes(BOARD_LIDAR_UART_NUM, s_rx, sizeof(s_rx),
                            pdMS_TO_TICKS(500));
    if (n <= 0) {
        ESP_LOGE(TAG, "No data — check Verde→GPIO%d", BOARD_LIDAR_UART_RX_GPIO);
        vTaskDelete(NULL);
        return;
    }
    int off = find_sp10_frame(s_rx, n);
    if (off < 0) {
        ESP_LOGE(TAG, "%d bytes received but no valid SP10 frame", n);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "SP10M01 streaming — first frame: %u mm",
             sp10_distance_mm(&s_rx[off]));

    // --- Bucle principal: lectura continua sincronizada por cabecera ---
    uint8_t f[SP10_FRAME_LEN];

    while (1) {
        // Leer un byte y descartar si no es la cabecera de trama.
        uint8_t b;
        if (uart_read_bytes(BOARD_LIDAR_UART_NUM, &b, 1, pdMS_TO_TICKS(100)) <= 0)
            continue;
        if (b != SP10_HEADER) continue;

        // Cabecera encontrada: leer los 3 bytes restantes de la trama.
        // Timeout corto (20 ms) porque los bytes de la misma trama llegan
        // en ráfaga inmediata a 460800 baud.
        if (uart_read_bytes(BOARD_LIDAR_UART_NUM, &f[1], SP10_FRAME_LEN - 1,
                            pdMS_TO_TICKS(20)) < SP10_FRAME_LEN - 1)
            continue;

        // Verificar integridad de la trama mediante checksum.
        f[0] = b;
        if ((uint8_t)(f[1] + f[2] + f[3]) != 0xFF) continue;

        // Trama válida: actualizar la distancia compartida y registrar en log.
        uint16_t dist_mm = sp10_distance_mm(f);
        s_distance_mm = dist_mm;

        ESP_LOGI(TAG, "dist=%u mm", dist_mm);
    }
}

// -----------------------------------------------------------------------------
// lidar_init — configura el UART y lanza la tarea de lectura del sensor.
//
// El UART se inicializa con los parámetros exactos del sensor SP10M01:
// 460800 baud, 8N1, sin control de flujo por hardware. Se asignan los pines
// RX y TX según board_config.h. El buffer de recepción es de 1024 bytes
// (2 × UART_BUF_SIZE) para absorber ráfagas de tramas sin pérdida de datos.
// -----------------------------------------------------------------------------
esp_err_t lidar_init(void)
{
    const uart_config_t cfg = {
        .baud_rate  = SP10_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(BOARD_LIDAR_UART_NUM, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(BOARD_LIDAR_UART_NUM,
                                 BOARD_LIDAR_UART_TX_GPIO,
                                 BOARD_LIDAR_UART_RX_GPIO,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(BOARD_LIDAR_UART_NUM,
                                        UART_BUF_SIZE * 2, 0, 0, NULL, 0));

    xTaskCreate(lidar_task, "lidar", 4096, NULL, 6, NULL);
    ESP_LOGI(TAG, "initialized UART%d  RX=GPIO%d (Verde)  TX=GPIO%d (Amarillo)",
             BOARD_LIDAR_UART_NUM,
             BOARD_LIDAR_UART_RX_GPIO,
             BOARD_LIDAR_UART_TX_GPIO);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// lidar_get_distance_mm — retorna la última distancia válida en milímetros.
//
// Retorna 0 si aún no se recibió ninguna trama (por ejemplo, recién arrancado
// el sistema o sensor desconectado). La tarea de proximidad en sentis.c trata
// el valor 0 como "sin datos" y mantiene los motores apagados.
// -----------------------------------------------------------------------------
uint16_t lidar_get_distance_mm(void)
{
    return s_distance_mm;
}
