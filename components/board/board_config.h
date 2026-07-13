#pragma once

// =============================================================================
// board_config.h — pin and peripheral assignments for SENTIS
// Plain integer constants only — no ESP-IDF headers needed.
// Each component includes its own driver headers.
// =============================================================================

// Phase 1 — Haptics (vibration motors via LEDC PWM)
#define BOARD_HAPTIC_LEFT_GPIO    3
#define BOARD_HAPTIC_RIGHT_GPIO   6

// Phase 3 — LiDAR SP10M01 (UART1, full-duplex, 3.3V)
// Cable colors on this unit: Verde=TX  Amarillo=RX  Negro=GND  Rojo=VCC(3V3)
// No level shifter needed — sensor and ESP32 both operate at 3.3V logic.
#define BOARD_LIDAR_UART_NUM      1       // UART_NUM_1
#define BOARD_LIDAR_UART_RX_GPIO  4       // sensor TX (Verde)   → ESP RX GPIO4
#define BOARD_LIDAR_UART_TX_GPIO  5       // ESP TX GPIO5        → sensor RX (Amarillo)
#define BOARD_LIDAR_UART_BAUD     115200

// Phase 2 — Audio (onboard ES8311 codec + NS4150B amplifier)
// All pins are internal — no external wiring needed.
#define BOARD_I2S_NUM             0       // I2S_NUM_0
#define BOARD_I2S_MCLK_GPIO       13
#define BOARD_I2S_SCLK_GPIO       12     // BCLK
#define BOARD_I2S_LRCK_GPIO       10     // WS / frame sync
#define BOARD_I2S_ASDOUT_GPIO     11     // Codec → ESP (recording/mic)
#define BOARD_I2S_DSDIN_GPIO      9      // ESP → Codec (playback)

// I2C control bus (shared: codec ES8311 + camera OV5647)
#define BOARD_I2C_NUM             0       // I2C_NUM_0
#define BOARD_I2C_SDA_GPIO        7
#define BOARD_I2C_SCL_GPIO        8
#define BOARD_CODEC_I2C_ADDR      0x18   // ES8311, CE pulled low

// Amplifier NS4150B enable (HIGH = on, LOW = mute)
#define BOARD_PA_CTRL_GPIO        53

// Phase 2 — MicroSD (onboard SDMMC 4-bit, SDIO 3.0)
// All pins are internal — no external wiring needed.
#define BOARD_SD_CLK_GPIO         43
#define BOARD_SD_CMD_GPIO         44
#define BOARD_SD_D0_GPIO          39
#define BOARD_SD_D1_GPIO          40
#define BOARD_SD_D2_GPIO          41
#define BOARD_SD_D3_GPIO          42

// Phase 5 — Camera OV5647 (MIPI CSI-2, onboard connector)
// CSI data lanes: dedicated pins, not in GPIO header.
// I2C control: shares BOARD_I2C_SDA_GPIO / BOARD_I2C_SCL_GPIO.
//
// MCLK/XCLK: GPIO54 es la salida de clock para la cámara en la ESP32-P4-Function-EV-Board.
// El OV5647 necesita 24 MHz de XCLK para generar su PLL MIPI. Si el módulo
// no tiene oscilador propio (la mayoría no lo tienen), sin este clock la cámara
// responde a I2C pero no transmite datos MIPI.
// Si el módulo tiene oscilador propio, se puede deshabilitar con BOARD_CAM_XCLK_GPIO = -1.
#define BOARD_CAM_XCLK_GPIO       54
#define BOARD_CAM_XCLK_FREQ_HZ    24000000

// Phase 7 — ESP32-C6 coprocessor (integrated on module via SDIO)
// No external wiring needed — excluded from prototype v1.

// =============================================================================
// DO NOT USE — reserved for onboard peripherals
// 7, 8       : I2C bus (codec + camera)
// 9–13       : I2S bus (codec)
// 37, 38     : UART0 (USB-UART debug, CH343P)
// 39–44      : SDMMC (microSD)
// 53         : PA_CTRL (amplifier NS4150B)
// =============================================================================
