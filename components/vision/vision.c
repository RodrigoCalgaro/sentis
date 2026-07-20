#include "vision.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_types.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_csi_brg_ll.h"
#include "driver/isp_core.h"
#include "ov5647.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "vision";

// =============================================================================
// Parámetros de captura — OV5647 modo MIPI RAW8 800×640 @50 fps
//
// El OV5647 en modo MIPI CSI-2 no soporta resoluciones inferiores a 800×640.
// Se usa el modo RAW8 más pequeño disponible para minimizar el tamaño del buffer
// y el tiempo de análisis. El buffer ocupa 800×640 = 512.000 bytes y debe
// residir en PSRAM (habilitar en menuconfig → Component config → ESP PSRAM).
//
// El análisis heurístico trabaja con submuestreo: analiza 1 de cada STEP_X
// columnas para limitar el tiempo de cómputo a < 5 ms con el ESP32-P4 a 400 MHz.
// =============================================================================
#define FRAME_W   800
#define FRAME_H   640
#define FRAME_SZ  (FRAME_W * FRAME_H)  // bytes, formato RAW8 (Bayer RGGB)

// Submuestreo horizontal: tomar 1 de cada N columnas en el análisis de bordes.
// Con STEP_X = 4 se procesa 1/4 del ancho real → tiempo equivalente a 200 col.
#define STEP_X    4

// =============================================================================
// Umbral de actividad de bordes
//
// "Actividad" de una franja = promedio de diferencias absolutas entre píxeles
// horizontales adyacentes muestreados (escala 0–255).
//
//   ~5  → ruido o textura sin bordes nítidos
//   ~12 → borde difuso (umbral de partida recomendado)
//   ~30 → borde nítido objeto–fondo
//
// Ajustar durante calibración. Un valor bajo genera falsos positivos;
// demasiado alto genera falsos negativos.
// =============================================================================
#define EDGE_THRESHOLD  5

// Tasas MIPI del modo seleccionado (800×640 @50fps):
//   IDI clock = 100 MHz → lane bit rate = 100 MHz × 4 = 400 Mbps
#define OV5647_LANE_BIT_RATE_MBPS  400

// Estado interno del componente.
static i2c_master_bus_handle_t  s_i2c_bus    = NULL;
static esp_sccb_io_handle_t     s_sccb_io    = NULL;
static esp_cam_ctlr_handle_t    s_cam_ctrl   = NULL;
static uint8_t                 *s_frame      = NULL;
static SemaphoreHandle_t        s_frame_sem  = NULL;

// Callbacks del driver CSI — llamados desde ISR, deben ser IRAM_ATTR.
//
// on_get_new_trans: el driver CSI llama esto en el ISR de cada frame para
//   obtener el buffer de destino del DMA. Retornamos siempre el mismo buffer
//   porque analizamos un frame a la vez.
//
// on_trans_finished: el driver CSI llama esto cuando el DMA terminó de llenar
//   el buffer. Señalizamos s_frame_sem para desbloquear vision_task.
//   El valor de retorno indica si se despertó una tarea de mayor prioridad
//   (para que FreeRTOS haga context switch al salir del ISR).
static IRAM_ATTR bool csi_get_new_trans_cb(esp_cam_ctlr_handle_t handle,
                                            esp_cam_ctlr_trans_t *trans,
                                            void *user_data)
{
    trans->buffer = s_frame;
    trans->buflen = FRAME_SZ;
    return false;
}

static IRAM_ATTR bool csi_trans_finished_cb(esp_cam_ctlr_handle_t handle,
                                             esp_cam_ctlr_trans_t *trans,
                                             void *user_data)
{
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &high_task_woken);
    return high_task_woken == pdTRUE;
}

// Resultado del último análisis. Volatile porque la escribe vision_task y la
// lee proximity_task. La escritura de un uint8_t es atómica en RISC-V.
static volatile obstacle_side_t s_side  = OBSTACLE_SIDE_NONE;
static volatile bool            s_ready = false;

// Buffer de display para el monitor de desarrollo. vision_task copia s_frame
// aquí después de cada análisis. El mutex garantiza que el monitor no lee
// mientras se copia, y que vision_task no escribe mientras el monitor codifica.
static uint8_t           *s_display_frame = NULL;
static SemaphoreHandle_t  s_display_mutex = NULL;

// -----------------------------------------------------------------------------
// analyze_frame — heurística de posición por actividad de bordes horizontales.
//
// Divide el frame en tres franjas verticales de igual ancho (izquierda, centro,
// derecha). En cada franja calcula el promedio de diferencias absolutas entre
// columnas adyacentes muestreadas cada STEP_X píxeles. Un borde nítido en el
// eje X produce una diferencia alta; un fondo uniforme produce diferencia ~0.
//
// Nota sobre RAW8 (Bayer RGGB):
//   Los píxeles pares de cada fila son R o G; los impares son G o B. Esta
//   mezcla de canales no afecta la detección de bordes porque las transiciones
//   en los bordes de un objeto se manifiestan en todos los canales de color.
// -----------------------------------------------------------------------------
static obstacle_side_t analyze_frame(const uint8_t *frame)
{
    const int zone_w  = FRAME_W / 3;
    // Transiciones muestreadas por franja: cada STEP_X columnas, todas las filas
    const int n_samp  = ((zone_w - 1) / STEP_X) * FRAME_H;

    uint32_t act[3] = {0};

    for (int y = 0; y < FRAME_H; y++) {
        const uint8_t *row = &frame[y * FRAME_W];
        // Paso de STEP_X para reducir carga; el borde se detecta igual porque
        // los cambios de intensidad en un borde real abarcan múltiples píxeles.
        for (int x = STEP_X; x < FRAME_W; x += STEP_X) {
            int diff = (int)row[x] - (int)row[x - STEP_X];
            if (diff < 0) diff = -diff;

            int zone = x / zone_w;
            if (zone > 2) zone = 2;
            act[zone] += (uint32_t)diff;
        }
    }

    uint32_t max_norm = 0;
    int      max_zone = 0;
    for (int z = 0; z < 3; z++) {
        uint32_t norm = (n_samp > 0) ? (act[z] / (uint32_t)n_samp) : 0;
        if (norm > max_norm) {
            max_norm = norm;
            max_zone = z;
        }
    }

    if (max_norm < EDGE_THRESHOLD) {
        return OBSTACLE_SIDE_NONE;
    }

    // El sensor OV5647 en esta placa entrega la imagen espejada horizontalmente:
    // la zona izquierda del buffer RAW8 corresponde al lado derecho de la escena real.
    // Se invierten LEFT/RIGHT para que la heurística coincida con la realidad.
    static const obstacle_side_t map[3] = {
        OBSTACLE_SIDE_RIGHT,   // zona 0 (izq. buffer) = derecha real
        OBSTACLE_SIDE_CENTER,
        OBSTACLE_SIDE_LEFT,    // zona 2 (der. buffer) = izquierda real
    };
    return map[max_zone];
}

// -----------------------------------------------------------------------------
// vision_task — captura frames continuamente y actualiza s_side.
// -----------------------------------------------------------------------------
static void vision_task(void *arg)
{
    static const char *side_names[] = {"NONE", "LEFT", "CENTER", "RIGHT"};

    while (1) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
            obstacle_side_t result = analyze_frame(s_frame);
            s_side  = result;
            s_ready = true;
            ESP_LOGD(TAG, "frame ok — side=%s", side_names[result]);

            // Copia no bloqueante al buffer de display para el monitor WiFi.
            // Si el monitor está codificando (mutex tomado), se omite este frame.
            if (s_display_frame &&
                xSemaphoreTake(s_display_mutex, 0) == pdTRUE) {
                memcpy(s_display_frame, s_frame, FRAME_SZ);
                xSemaphoreGive(s_display_mutex);
            }
        } else {
            ESP_LOGW(TAG, "frame timeout");
        }
    }
}

// -----------------------------------------------------------------------------
// vision_init — inicializa el pipeline de cámara y arranca vision_task.
//
// Secuencia:
//   1. MCLK/XCLK para el OV5647 (GPIO54, 24 MHz vía clock router)
//   2. Bus I2C master (compartido con ES8311 en Fase 2 — ver nota más abajo)
//   2. Handle SCCB sobre I2C (capa de control del sensor OV5647)
//   3. Detección del OV5647 y selección del formato RAW8 800×640 @50fps
//   4. Controlador MIPI CSI-2 del ESP32-P4
//   5. Streaming encendido
//   6. Buffer de frame en PSRAM
//   7. vision_task
//
// Nota I2C compartido:
//   El bus I2C_NUM_0 (GPIO7/8) será requerido también por el codec ES8311
//   en la Fase 2 (audio). Cuando eso suceda, el handle s_i2c_bus deberá
//   moverse a un componente board compartido para evitar doble inicialización
//   del mismo puerto I2C.
// -----------------------------------------------------------------------------
esp_err_t vision_init(void)
{
    esp_err_t ret;

    // -------------------------------------------------------------------------
    // 0. LDO interno para el MIPI CSI PHY
    //
    // El PHY MIPI-CSI del ESP32-P4 (receptor de lanes) necesita que el LDO
    // interno #3 esté activo a 2500 mV para operar. Sin esta alimentación el
    // PHY no puede decodificar la señal diferencial del OV5647 y el CSI bridge
    // nunca recibe datos — el DMA nunca dispara.
    //
    // Referencia: esp-video-components/esp_video/src/device/esp_video_csi_device.c
    //   CSI_LDO_UNIT_ID = 3, CSI_LDO_CFG_VOL_MV = 2500
    // -------------------------------------------------------------------------
    {
        esp_ldo_channel_handle_t ldo_handle = NULL;
        const esp_ldo_channel_config_t ldo_cfg = {
            .chan_id    = 3,
            .voltage_mv = 2500,
        };
        ret = esp_ldo_acquire_channel(&ldo_cfg, &ldo_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "MIPI CSI LDO init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_LOGI(TAG, "MIPI CSI PHY LDO channel 3 @ 2500mV ON");
    }

    // -------------------------------------------------------------------------
    // 1. MCLK/XCLK para el OV5647
    //
    // El OV5647 necesita 24 MHz de clock externo para su PLL MIPI. Sin él
    // el sensor responde a I2C pero no genera señal en los data lanes MIPI.
    // En la ESP32-P4-Function-EV-Board este clock sale por GPIO54 vía el
    // clock router interno del SoC (SPLL 480 MHz / 20 = 24 MHz).
    //
    // Si el módulo tiene oscilador propio, este init es innecesario pero
    // inofensivo (el GPIO simplemente queda sin conectar eléctricamente).
    // -------------------------------------------------------------------------
    if (BOARD_CAM_XCLK_GPIO >= 0) {
        esp_cam_sensor_xclk_handle_t xclk_handle = NULL;
        ret = esp_cam_sensor_xclk_allocate(ESP_CAM_SENSOR_XCLK_ESP_CLOCK_ROUTER, &xclk_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "XCLK alloc failed: %s", esp_err_to_name(ret));
            return ret;
        }
        const esp_cam_sensor_xclk_config_t xclk_cfg = {
            .esp_clock_router_cfg = {
                .xclk_pin     = BOARD_CAM_XCLK_GPIO,
                .xclk_freq_hz = BOARD_CAM_XCLK_FREQ_HZ,
            }
        };
        ret = esp_cam_sensor_xclk_start(xclk_handle, &xclk_cfg);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "XCLK start failed: %s", esp_err_to_name(ret));
            return ret;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
        ESP_LOGI(TAG, "XCLK %d MHz → GPIO%d", BOARD_CAM_XCLK_FREQ_HZ / 1000000, BOARD_CAM_XCLK_GPIO);
    }

    // -------------------------------------------------------------------------
    // 2. Bus I2C master
    // -------------------------------------------------------------------------
    const i2c_master_bus_config_t i2c_cfg = {
        .i2c_port          = BOARD_I2C_NUM,
        .sda_io_num        = BOARD_I2C_SDA_GPIO,
        .scl_io_num        = BOARD_I2C_SCL_GPIO,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&i2c_cfg, &s_i2c_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // -------------------------------------------------------------------------
    // 2. Handle SCCB sobre I2C
    //    El OV5647 usa la dirección 0x36 con registros de 16 bits.
    //    SCCB es el protocolo I2C propietario de OmniVision; en ESP-IDF se
    //    accede a través del componente espressif/esp_sccb_intf.
    // -------------------------------------------------------------------------
    const sccb_i2c_config_t sccb_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = OV5647_SCCB_ADDR,   // 0x36
        .scl_speed_hz    = 100000,              // 100 kHz estándar SCCB
    };
    ret = sccb_new_i2c_io(s_i2c_bus, &sccb_cfg, &s_sccb_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SCCB I2C IO init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // -------------------------------------------------------------------------
    // 3. Detectar OV5647 y seleccionar formato
    //    ov5647_detect() realiza el probe I2C internamente y devuelve NULL si
    //    el sensor no responde. A continuación se consultan los formatos
    //    disponibles y se selecciona el modo RAW8 de menor resolución (800×640).
    // -------------------------------------------------------------------------
    esp_cam_sensor_config_t sensor_cfg = {
        .sccb_handle  = s_sccb_io,
        .reset_pin    = -1,
        .pwdn_pin     = -1,
        .xclk_pin     = -1,
        .xclk_freq_hz = 0,
        .sensor_port  = ESP_CAM_SENSOR_MIPI_CSI,
    };
    esp_cam_sensor_device_t *sensor = ov5647_detect(&sensor_cfg);
    if (!sensor) {
        ESP_LOGE(TAG, "OV5647 not found on I2C%d (SDA=GPIO%d SCL=GPIO%d addr=0x%02X)",
                 BOARD_I2C_NUM, BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, OV5647_SCCB_ADDR);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "OV5647 detected");

    // Consultar formatos disponibles del sensor y seleccionar el de menor
    // resolución RAW8 en modo MIPI para minimizar el tamaño del buffer.
    esp_cam_sensor_format_array_t fmt_array = {0};
    ESP_ERROR_CHECK(esp_cam_sensor_query_format(sensor, &fmt_array));

    const esp_cam_sensor_format_t *selected = NULL;
    for (int i = 0; i < (int)fmt_array.count; i++) {
        const esp_cam_sensor_format_t *f = &fmt_array.format_array[i];
        if (f->format != ESP_CAM_SENSOR_PIXFORMAT_RAW8) continue;
        if (f->port   != ESP_CAM_SENSOR_MIPI_CSI)       continue;
        if (!selected ||
            (f->width * f->height < selected->width * selected->height)) {
            selected = f;
        }
    }
    if (!selected) {
        ESP_LOGE(TAG, "no RAW8 MIPI format found in OV5647 driver");
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "selected format: %s (%ux%u @%ufps)",
             selected->name, selected->width, selected->height, selected->fps);

    ESP_ERROR_CHECK(esp_cam_sensor_set_format(sensor, selected));

    // -------------------------------------------------------------------------
    // 4. Controlador MIPI CSI-2
    //    h_res / v_res del formato seleccionado (800×640 en modo RAW8 mínimo).
    //    lane_bit_rate_mbps: IDI clock (100 MHz) × 4 = 400 Mbps para los modos
    //    800×N del OV5647.
    // -------------------------------------------------------------------------
    const esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .clk_src                = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res                  = selected->width,
        .v_res                  = selected->height,
        .data_lane_num          = selected->mipi_info.lane_num,
        .lane_bit_rate_mbps     = OV5647_LANE_BIT_RATE_MBPS,
        .input_data_color_type  = CAM_CTLR_COLOR_RAW8,
        .output_data_color_type = CAM_CTLR_COLOR_RAW8,
        .queue_items            = 1,
        .bk_buffer_dis          = 1,  // usamos nuestro propio buffer en receive()
    };
    ret = esp_cam_new_csi_ctlr(&csi_cfg, &s_cam_ctrl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI controller init failed: %s", esp_err_to_name(ret));
        return ret;
    }

#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    // -------------------------------------------------------------------------
    // Fix v1.x: configurar dmablk_size del CSI bridge.
    //
    // En v1.x el registro dmablk_size (default 8191) define cuántos bursts DMA
    // constituyen un bloque. El CSI bridge afirma dma_last_req después de
    // dmablk_size * burst_len transferencias, señalizando al DW-GDMA que el
    // bloque completó. Con el default de 8191 y burst_len=512:
    //   bloque = 8191 * 512 * 8 bytes = 33.5 MB (nunca se alcanza para 512 KB)
    //
    // Valor correcto = frame_size_64bit / burst_len = 64000 / 512 = 125
    // Así el DW-GDMA recibe dma_last_req exactamente al final de cada frame.
    // -------------------------------------------------------------------------
    {
        csi_brg_dev_t *brg = MIPI_CSI_BRG_LL_GET_HW(0);
        const uint32_t burst_len   = 512;
        const uint32_t frame_words = FRAME_SZ / 8;  // bytes → 64-bit words
        const uint32_t blk_size    = frame_words / burst_len;
        brg->dmablk_size.dmablk_size = blk_size;
        ESP_LOGI(TAG, "v1.x CSI bridge dmablk_size=%"PRIu32" (%"PRIu32" bursts × %"PRIu32" words = %"PRIu32" bytes)",
                 blk_size, blk_size, burst_len, blk_size * burst_len * 8);
    }
#endif

    // -------------------------------------------------------------------------
    // 5. Buffer de display para el monitor WiFi (solo desarrollo)
    //    Mismo tamaño que s_frame. Si no hay PSRAM suficiente, simplemente no
    //    se habilita el monitor — el resto del sistema sigue funcionando.
    // -------------------------------------------------------------------------
    s_display_mutex = xSemaphoreCreateMutex();
    if (s_display_mutex) {
        s_display_frame = heap_caps_malloc(FRAME_SZ,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_display_frame) {
            ESP_LOGW(TAG, "sin PSRAM para display frame — monitor WiFi deshabilitado");
        }
    }

    // -------------------------------------------------------------------------
    // 5. Semáforo de frame y buffer en PSRAM
    //    El semáforo sincroniza el ISR (csi_trans_finished_cb) con vision_task.
    //    El buffer debe asignarse antes de registrar los callbacks porque
    //    csi_get_new_trans_cb lo referencia desde el primer frame.
    //    800×640 RAW8 = 512.000 bytes — requiere PSRAM.
    // -------------------------------------------------------------------------
    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) {
        ESP_LOGE(TAG, "failed to create frame semaphore");
        return ESP_ERR_NO_MEM;
    }

    s_frame = heap_caps_malloc(FRAME_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
    if (!s_frame) {
        ESP_LOGW(TAG, "PSRAM no disponible, intentando SRAM interna (%d bytes)", FRAME_SZ);
        s_frame = heap_caps_malloc(FRAME_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    }
    if (!s_frame) {
        ESP_LOGE(TAG, "sin memoria para el frame buffer (%d bytes) — "
                      "habilitar PSRAM en menuconfig", FRAME_SZ);
        return ESP_ERR_NO_MEM;
    }

    // -------------------------------------------------------------------------
    // 6. Registrar callbacks del controlador CSI
    //    El driver CSI requiere on_trans_finished antes de esp_cam_ctlr_start().
    //    on_get_new_trans provee el buffer de destino para cada frame DMA.
    // -------------------------------------------------------------------------
    const esp_cam_ctlr_evt_cbs_t csi_cbs = {
        .on_get_new_trans  = csi_get_new_trans_cb,
        .on_trans_finished = csi_trans_finished_cb,
    };
    ret = esp_cam_ctlr_register_event_callbacks(s_cam_ctrl, &csi_cbs, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CSI callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // -------------------------------------------------------------------------
    // 7. Habilitar controlador CSI
    // -------------------------------------------------------------------------
    ESP_ERROR_CHECK(esp_cam_ctlr_enable(s_cam_ctrl));

    // -------------------------------------------------------------------------
    // 8. ISP en modo bypass
    //
    // En ESP32-P4, el flujo de datos MIPI es:
    //   MIPI Host → ISP → CSI Bridge → DW-GDMA → s_frame
    //
    // El ISP se interpone entre el host MIPI y el CSI bridge. Sin inicializarlo,
    // los datos nunca llegan al bridge (int_raw=0x00000000). Con bypass_isp=1
    // los datos pasan sin procesamiento (RAW8 → RAW8 directo).
    //
    // Referencia: esp_video_csi_device.c → start_isp() y
    //             examples/peripherals/camera/mipi_isp_dsi/main/mipi_isp_dsi_main.c
    // -------------------------------------------------------------------------
    isp_proc_handle_t isp_proc = NULL;
    {
        // bypass_isp=0 con RAW8→RAW8: el ISP debe quedar HABILITADO para que
        // los datos fluyan desde el MIPI Host al CSI bridge. Con bypass_isp=1
        // el driver bloquea esp_isp_enable() y los datos no llegan al bridge.
        // Con bypass_isp=0 y RAW8 in/out, el ISP actúa como pass-through sin
        // procesar el Bayer (demosaic deshabilitado para RAW output).
        const esp_isp_processor_cfg_t isp_cfg = {
            .clk_hz              = 80 * 1000 * 1000,
            .input_data_source   = ISP_INPUT_DATA_SOURCE_CSI,
            .input_data_color_type  = ISP_COLOR_RAW8,
            .output_data_color_type = ISP_COLOR_RAW8,
            .has_line_start_packet  = true,   // OV5647_CSI_LINESYNC_ENABLE=y
            .has_line_end_packet    = false,
            .h_res       = FRAME_W,
            .v_res       = FRAME_H,
            .bayer_order = COLOR_RAW_ELEMENT_ORDER_RGGB,
        };
        ret = esp_isp_new_processor(&isp_cfg, &isp_proc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ISP init failed: %s", esp_err_to_name(ret));
            return ret;
        }
        ESP_ERROR_CHECK(esp_isp_enable(isp_proc));
        ESP_LOGI(TAG, "ISP enabled (RAW8 pass-through, pipeline inactive)");
    }

    // -------------------------------------------------------------------------
    // 9. Arrancar el controlador CSI y encender el streaming del sensor
    // -------------------------------------------------------------------------
    ESP_ERROR_CHECK(esp_cam_ctlr_start(s_cam_ctrl));

    int stream_on = 1;
    ESP_ERROR_CHECK(esp_cam_sensor_ioctl(sensor, ESP_CAM_SENSOR_IOC_S_STREAM, &stream_on));

    // -------------------------------------------------------------------------
    // 8. Tarea de captura y análisis
    //    Prioridad 4: menor que lidar_task (6) y proximity_task (5).
    // -------------------------------------------------------------------------
    xTaskCreate(vision_task, "vision", 4096, NULL, 4, NULL);

    ESP_LOGI(TAG, "initialized — %ux%u RAW8 @%ufps, step=%d, edge_thr=%d",
             selected->width, selected->height, selected->fps, STEP_X, EDGE_THRESHOLD);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// Getters — lectura atómica del estado compartido.
// -----------------------------------------------------------------------------
obstacle_side_t vision_get_obstacle_side(void)
{
    return s_side;
}

bool vision_is_ready(void)
{
    return s_ready;
}

bool vision_copy_display_frame(uint8_t *dst, size_t len)
{
    if (!dst || len < FRAME_SZ || !s_display_frame || !s_ready) return false;
    if (xSemaphoreTake(s_display_mutex, pdMS_TO_TICKS(50)) != pdTRUE) return false;
    memcpy(dst, s_display_frame, FRAME_SZ);
    xSemaphoreGive(s_display_mutex);
    return true;
}
