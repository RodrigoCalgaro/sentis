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
// SP10M01 — single-point DToF lidar, full-duplex UART 3.3V, 460800 baud.
// Cable: Verde=TX(sensor)→GPIO4  Amarillo=RX(sensor)→GPIO5  Negro=GND  Rojo=3V3
//
// Protocol (reverse-engineered + calibrated, 2026-06-16):
//   - Sensor auto-streams at 460800 baud; no start command needed.
//   - Frame: 4 bytes  [5C] [dist_low] [dist_high] [check]
//       5C         = fixed header
//       dist_low   = distance low byte  (mm, little-endian)
//       dist_high  = distance high byte (mm, little-endian)
//       check      = 0xFF - dist_low - dist_high
//   distance_mm = dist_low | (dist_high << 8)
//   Calibration confirmed: object at 24 cm → data=0xF4 → 244 mm ✓
// =============================================================================

#define SP10_HEADER    0x5C
#define SP10_FRAME_LEN 4
#define SP10_BAUD      460800

#define UART_BUF_SIZE  512

static volatile uint16_t s_distance_mm = 0;
static uint8_t s_rx[256];

// Return index of the first valid SP10 frame in buf[0..n-1], or -1.
static int find_sp10_frame(const uint8_t *buf, int n)
{
    for (int i = 0; i + SP10_FRAME_LEN <= n; i++) {
        if (buf[i] == SP10_HEADER &&
            (uint8_t)(buf[i+1] + buf[i+2] + buf[i+3]) == 0xFF)
            return i;
    }
    return -1;
}

static uint16_t sp10_distance_mm(const uint8_t *frame)
{
    return (uint16_t)frame[1] | ((uint16_t)frame[2] << 8);
}

static void lidar_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(500));

    // Confirm at least one valid frame before entering the stream loop.
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

    uint8_t f[SP10_FRAME_LEN];
    int logged = 0;

    while (1) {
        uint8_t b;
        if (uart_read_bytes(BOARD_LIDAR_UART_NUM, &b, 1, pdMS_TO_TICKS(100)) <= 0)
            continue;
        if (b != SP10_HEADER) continue;

        if (uart_read_bytes(BOARD_LIDAR_UART_NUM, &f[1], SP10_FRAME_LEN - 1,
                            pdMS_TO_TICKS(20)) < SP10_FRAME_LEN - 1)
            continue;

        f[0] = b;
        if ((uint8_t)(f[1] + f[2] + f[3]) != 0xFF) continue;

        uint16_t dist_mm = sp10_distance_mm(f);
        s_distance_mm = dist_mm;
        if (logged < 10) {
            ESP_LOGI(TAG, "dist=%u mm", dist_mm);
            logged++;
        }
    }
}

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

uint16_t lidar_get_distance_mm(void)
{
    return s_distance_mm;
}
