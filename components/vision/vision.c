#include "vision.h"
#include "board_config.h"
#include "bus.h"
#include "driver/i2c_master.h"
#include "esp_sccb_i2c.h"
#include "esp_sccb_intf.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_sensor.h"
#include "esp_cam_sensor_types.h"
#include "esp_cam_sensor_xclk.h"
#include "esp_ldo_regulator.h"
#include "hal/mipi_csi_brg_ll.h"
#include "driver/isp_core.h"
#include "driver/isp_demosaic.h"
#include "ov5647.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "vision";

// =============================================================================
// Parámetros de captura — OV5647 modo MIPI RAW10 1280×960 binning @45 fps
//
// Historial: 800×640 (Fase 5, mínimo del sensor) → insuficiente para OCR →
// 800×1280 RAW8 (más resolución vertical) → resultó "zoomeado": ese modo
// recorta la ventana del sensor a solo ~74% del alto activo (VSTA=248,
// VWIN=1447 de 1944 filas — ver ov5647_input_24M_MIPI_2lane_raw8_800x1280_50fps
// en ov5647_settings.h) porque está pensado para alimentar el panel MIPI-DSI
// portrait de esta placa (mismo 800x1280 que TEST_MIPI_DSI_DISP_VRES en el
// test oficial del driver), no para capturar el FOV completo.
//
// 1280×960 en cambio usa binning 2×2 real sobre ~99% del área activa del
// sensor en ambos ejes (X: 24–2600 de 2592, Y: 12–1944 de 1944 — ver
// ov5647_input_24M_MIPI_2lane_raw10_1280x960_45fps), así que recupera el FOV
// completo Y promedia píxeles vecinos en vez de sub-muestrear como los modos
// RAW8 anteriores (mejor relación señal/ruido). Contrapartida: el sensor solo
// ofrece este binning en RAW10 (10 bits empaquetados en el lane MIPI), así
// que el CSI/ISP tienen que reconfigurarse de RAW8 a RAW10 — ver
// CAM_CTLR_COLOR_RAW10 / ISP_COLOR_RAW10 más abajo. El ISP sigue entregando
// RGB565 (2 bytes/píxel) al buffer final independientemente del bit-depth de
// entrada, así que FRAME_SZ no cambia de fórmula.
//
// PSRAM: con este modo + monitor de desarrollo habilitado, los buffers de
// cámara/OCR/monitor rondan ~6 MB de delta sobre 800×640 — validar con el log
// "PSRAM libre tras init" en sentis.c antes de asumir que entra sin ajustes.
// =============================================================================
#define FRAME_W   1280
#define FRAME_H   960
#define FRAME_SZ  (FRAME_W * FRAME_H * 2)  // bytes, formato RGB565 (2 B/píxel, tras demosaico ISP)

// Submuestreo horizontal: tomar 1 de cada N columnas en el análisis de bordes.
// Con STEP_X = 4 se procesa 1/4 del ancho real → tiempo equivalente a 200 col.
#define STEP_X    4

// Target de brillo del AE (rango 2-235, ver ov5647_set_AE_target). El default
// del driver es 0x50=80 (~34% de escala) y en este modo de binning resultó
// insuficiente en una habitación con buena luz — hacía falta linterna de
// celular para exponer bien. Experimento: subirlo para forzar más exposición/
// ganancia. Ajustar este valor si sale sobre o sub-expuesto.
//
// 235 (el máximo) causó watchdog timeout en IDLE1/CPU1 con vision_task
// atascado sin ceder CPU. Sospecha: ganancia al máximo degrada la señal MIPI
// en este modo de binning, ya delicado (ver has_line_start_packet más abajo),
// y el controlador CSI dispara "frame terminado" más seguido de lo real.
//
// RETEST 2026-09-08: con AE_TARGET=235, "ISP: fifo overflow" y el panic por
// interrupt watchdog en CPU0 aparecen siempre al mismo tiempo exacto desde
// que arranca la cámara (~28.5s), reproducible incluso con la arquitectura de
// captura de doble buffer (ver s_frame_buf más arriba) que eliminó la
// condición de carrera constante que había antes — la consistencia del
// timing, independiente de cambios de software, es consistente con que sea
// el AE convergiendo a ganancia máxima en vez de un bug de vision_task.
// Volviendo a 180 (el valor documentado como estable) para confirmar.
#define AE_TARGET 180

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

// La tasa MIPI (lane_bit_rate_mbps) ya no se hardcodea: 1280×960 usa un IDI
// clock distinto (88,33 MHz × 5 ≈ 441,7 Mbps, ver OV5647_MIPI_CSI_LINE_RATE_
// 1280x960_45FPS en ov5647_settings.h) al de los modos RAW8 anteriores
// (100 MHz × 4 = 400 Mbps) — se lee directamente de selected->mipi_info.mipi_clk
// más abajo para no volver a desincronizar un valor a mano con cada cambio de
// modo (ver uso en csi_cfg.lane_bit_rate_mbps).

// Estado interno del componente.
static i2c_master_bus_handle_t  s_i2c_bus    = NULL;
static esp_sccb_io_handle_t     s_sccb_io    = NULL;
static esp_cam_ctlr_handle_t    s_cam_ctrl   = NULL;
static SemaphoreHandle_t        s_frame_sem  = NULL;

// -----------------------------------------------------------------------------
// Doble buffer de captura (ping-pong) — reemplaza al buffer único + copia a
// s_display_frame que tenía este archivo antes.
//
// Diagnóstico previo (medido en hardware): con un solo buffer de captura
// (queue_items=1, sin doble buffer) el DMA del CSI reescribe el mismo buffer
// sin parar, incluso mientras vision_task todavía lo está leyendo para
// analyze_frame() y para la copia al buffer de display — una condición de
// carrera en TODOS los frames, no ocasional. vision_task tarda ~40-100ms por
// vuelta (analyze_frame + memcpy), varias veces más que el hueco real entre
// frames del sensor (~22-33ms), así que la lectura nunca termina antes de que
// el DMA vuelva a escribir encima. Esto explica tanto el "ISP: fifo overflow"
// como el framerate muy por debajo del esperado — confirmado descartando por
// evidencia de hardware: coexistencia de monitores, AE_TARGET, fps del sensor
// (VTS) y el patrón de acceso de analyze_frame(), ninguno de los cuales tuvo
// efecto por separado.
//
// s_frame_buf[2] resuelve la causa de raíz: dos buffers alternados, uno
// siempre "en captura" (el DMA escribe ahí) y el otro "listo" (contiene el
// último frame completo y estable). Los callbacks del driver CSI deciden el
// destino de la PRÓXIMA captura en on_get_new_trans, que el driver llama
// ANTES de on_trans_finished dentro del mismo evento ISR (confirmado leyendo
// esp_cam_ctlr_csi.c: csi_dma_trans_done_callback llama primero
// on_get_new_trans, arma el DMA con ese buffer, y recién después llama
// on_trans_finished con el buffer que se acaba de completar) — por eso el
// toggle de s_capture_idx vive en on_get_new_trans, no en on_trans_finished.
//
// s_ready_idx (el índice con el frame listo) es un entero volatile de una
// sola palabra, sin mutex: el mismo patrón que ya usa este archivo para
// s_side/s_ready. Como el buffer "listo" no vuelve a ser blanco del DMA hasta
// completar un ciclo entero del otro buffer (~1 frame real, ~22-33ms según el
// fps configurado), un lector que termine su copia dentro de ese margen no
// puede pisarse con la próxima escritura — sin necesidad de un contador de
// lectores. No es una garantía matemática absoluta bajo cualquier atraso de
// software, pero elimina la carrera permanente que había antes (100% de los
// frames) y no agrega PSRAM: mismo total que antes (s_frame + s_display_frame
// = 2 × FRAME_SZ).
// -----------------------------------------------------------------------------
static uint8_t      *s_frame_buf[2]  = {NULL, NULL};
static volatile int   s_capture_idx  = 0;   // buffer que el DMA está llenando (o va a llenar)
static volatile int   s_ready_idx    = -1;  // buffer con el último frame completo; -1 = ninguno aún

// Callbacks del driver CSI — llamados desde ISR, deben ser IRAM_ATTR.
//
// on_get_new_trans: el driver llama esto ANTES de on_trans_finished (mismo
//   evento) para decidir el destino de la PRÓXIMA captura — acá se hace el
//   toggle del ping-pong, siempre hacia el buffer contrario al que se está
//   por reportar terminado.
//
// on_trans_finished: el driver llama esto con el buffer que el DMA acaba de
//   terminar de llenar — por construcción del ping-pong, es el índice
//   contrario al s_capture_idx recién actualizado por on_get_new_trans.
//   Señalizamos s_frame_sem para desbloquear vision_task. El valor de retorno
//   indica si se despertó una tarea de mayor prioridad (para que FreeRTOS
//   haga context switch al salir del ISR).
static IRAM_ATTR bool csi_get_new_trans_cb(esp_cam_ctlr_handle_t handle,
                                            esp_cam_ctlr_trans_t *trans,
                                            void *user_data)
{
    s_capture_idx ^= 1;
    trans->buffer = s_frame_buf[s_capture_idx];
    trans->buflen = FRAME_SZ;
    return false;
}

static IRAM_ATTR bool csi_trans_finished_cb(esp_cam_ctlr_handle_t handle,
                                             esp_cam_ctlr_trans_t *trans,
                                             void *user_data)
{
    s_ready_idx = s_capture_idx ^ 1;

    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_sem, &high_task_woken);
    return high_task_woken == pdTRUE;
}

// Resultado del último análisis. Volatile porque la escribe vision_task y la
// lee proximity_task. La escritura de un uint8_t es atómica en RISC-V.
static volatile obstacle_side_t s_side  = OBSTACLE_SIDE_NONE;
static volatile bool            s_ready = false;

// Pausa la heurística analyze_frame() sin detener captura/display — ver
// vision_set_analysis_paused() en vision.h. Volatile: la escribe ocr_task,
// la lee vision_task.
static volatile bool s_analysis_paused = false;

// Extrae un proxy de luminancia barato de un píxel RGB565 (RRRRRGGGGGGBBBBB)
// usando solo el canal verde (6 bits, el más cercano a luma) — evita floats
// y multiplicaciones, suficiente para la heurística de actividad de bordes.
static inline uint8_t rgb565_luma_proxy(uint16_t px)
{
    return (uint8_t)(((px >> 5) & 0x3F) << 2);
}

// Scratch de una fila completa (RAM interna) para analyze_frame(). frame vive
// en PSRAM: leerlo salteado (cada STEP_X píxeles) directo desde ahí rompe el
// acceso en ráfaga — medido en hardware: ~39ms/frame, un orden de magnitud
// más de lo esperado para ~307K muestras de aritmética simple, y suficiente
// por sí solo (sumado al memcpy de display) para explicar el tope de ~10fps
// medido vía DIAG. Mismo fix que vision_copy_display_frame_scaled(): traer la
// fila completa con un memcpy secuencial y muestrear STEP_X ahí.
static uint16_t s_analyze_row_scratch[FRAME_W];

// -----------------------------------------------------------------------------
// analyze_frame — heurística de posición por actividad de bordes horizontales.
//
// Divide el frame en tres franjas verticales de igual ancho (izquierda, centro,
// derecha). En cada franja calcula el promedio de diferencias absolutas entre
// columnas adyacentes muestreadas cada STEP_X píxeles. Un borde nítido en el
// eje X produce una diferencia alta; un fondo uniforme produce diferencia ~0.
// -----------------------------------------------------------------------------
static obstacle_side_t analyze_frame(const uint8_t *frame)
{
    const int zone_w  = FRAME_W / 3;
    // Transiciones muestreadas por franja: cada STEP_X columnas, todas las filas
    const int n_samp  = ((zone_w - 1) / STEP_X) * FRAME_H;

    uint32_t act[3] = {0};

    for (int y = 0; y < FRAME_H; y++) {
        const uint16_t *src_row = (const uint16_t *)(frame + (size_t)y * FRAME_W * 2);
        memcpy(s_analyze_row_scratch, src_row, (size_t)FRAME_W * 2);  // PSRAM->SRAM, secuencial
        // Paso de STEP_X para reducir carga; el borde se detecta igual porque
        // los cambios de intensidad en un borde real abarcan múltiples píxeles.
        for (int x = STEP_X; x < FRAME_W; x += STEP_X) {
            int diff = (int)rgb565_luma_proxy(s_analyze_row_scratch[x]) - (int)rgb565_luma_proxy(s_analyze_row_scratch[x - STEP_X]);
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
    // la zona izquierda del buffer corresponde al lado derecho de la escena real.
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
    // static int dbg_count = 0;  // solo usado por el log RGB565 comentado más abajo

    // DIAG — cuenta cuántas veces se señaliza s_frame_sem por segundo. Con el
    // doble buffer (ver comentario de s_frame_buf arriba) esto ya refleja el
    // framerate real de captura, no la velocidad del software.
    uint32_t frame_count     = 0;
    int64_t  last_report_us  = esp_timer_get_time();
    int64_t  analyze_us_sum  = 0;

    while (1) {
        if (xSemaphoreTake(s_frame_sem, pdMS_TO_TICKS(500)) == pdTRUE) {
            frame_count++;

            // Yield defensivo: garantiza que IDLE1 tenga oportunidad de
            // correr en cada vuelta, sea cual sea el ritmo real del DMA.
            vTaskDelay(1);

            int idx = s_ready_idx;
            if (idx >= 0) {
                // Mientras dura una lectura OCR, s_side no se consulta
                // (proximity_task lo ignora — ver ocr_is_reading() en
                // main/sentis.c), así que nos ahorramos el costo de CPU de
                // analyze_frame() sobre el frame completo. La captura CSI
                // sigue activa, para que vision_copy_display_frame() (que usa
                // ocr_task) siga entregando frames frescos del buffer listo.
                int64_t t_analyze_start = esp_timer_get_time();
                if (!s_analysis_paused) {
                    obstacle_side_t result = analyze_frame(s_frame_buf[idx]);
                    s_side  = result;
                    s_ready = true;
                    ESP_LOGD(TAG, "frame ok — side=%s", side_names[result]);
                } else {
                    s_ready = true;
                }
                analyze_us_sum += esp_timer_get_time() - t_analyze_start;
            }

            int64_t now_us = esp_timer_get_time();
            if (now_us - last_report_us >= 1000000) {
                ESP_LOGI(TAG, "%" PRIu32 " frames/s | analyze=%" PRId64 "us/frame",
                         frame_count,
                         frame_count ? analyze_us_sum / frame_count : 0);
                frame_count     = 0;
                analyze_us_sum  = 0;
                last_report_us  = now_us;
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
//   3. Detección del OV5647 y selección del formato RAW10 1280×960 @45fps
//   4. Controlador MIPI CSI-2 del ESP32-P4
//   5. Streaming encendido
//   6. Buffer de frame en PSRAM
//   7. vision_task
//
// Bus I2C compartido con Fase 2:
//   El bus I2C_NUM_0 (GPIO7/8) es gestionado por el componente bus (bus.h).
//   Tanto vision_init() como audio_init() llaman a bus_i2c_init() que es
//   idempotente — el periférico se crea una sola vez y ambos componentes
//   agregan sus dispositivos (OV5647 @ 0x36, ES8311 @ 0x18) al mismo bus.
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
    // 2. Bus I2C master (compartido con audio — componente bus)
    //    bus_i2c_init() es idempotente: si audio_init() ya lo creó, reutiliza
    //    el mismo handle sin re-inicializar el periférico.
    // -------------------------------------------------------------------------
    ret = bus_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    s_i2c_bus = bus_get_i2c();

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
    //    disponibles y se selecciona el modo RAW10 binning 1280×960 (ver
    //    comentario de parámetros de captura al inicio del archivo).
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

    // Consultar formatos disponibles del sensor y seleccionar el modo RAW10
    // MIPI que coincide exactamente con FRAME_W×FRAME_H (ver comentario de
    // parámetros de captura más arriba). Se matchea por dimensiones en vez de
    // "el más chico" porque se apunta a un modo específico (binning), no al
    // de menor resolución.
    esp_cam_sensor_format_array_t fmt_array = {0};
    ESP_ERROR_CHECK(esp_cam_sensor_query_format(sensor, &fmt_array));

    const esp_cam_sensor_format_t *selected = NULL;
    for (int i = 0; i < (int)fmt_array.count; i++) {
        const esp_cam_sensor_format_t *f = &fmt_array.format_array[i];
        if (f->format != ESP_CAM_SENSOR_PIXFORMAT_RAW10) continue;
        if (f->port   != ESP_CAM_SENSOR_MIPI_CSI)        continue;
        if (f->width  != FRAME_W || f->height != FRAME_H) continue;
        selected = f;
        break;
    }
    if (!selected) {
        ESP_LOGE(TAG, "no RAW10 MIPI %dx%d format found in OV5647 driver", FRAME_W, FRAME_H);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "selected format: %s (%ux%u @%ufps)",
             selected->name, selected->width, selected->height, selected->fps);

    ESP_ERROR_CHECK(esp_cam_sensor_set_format(sensor, selected));

    // -------------------------------------------------------------------------
    // Override manual de VTS — bajar el fps real del sensor sin cambiar
    // resolución ni binning.
    //
    // El modo RAW10 1280×960 binning viene fijo a 45fps en el driver del
    // OV5647 (ov5647_input_24M_MIPI_2lane_raw10_1280x960_45fps) — no existe
    // en el driver una variante de esta misma resolución a menor fps. A ese
    // pixel-rate (1280×960×45 ≈ 55.3 Mpx/s, más del doble que el modo
    // 800×640@50fps original ≈25.6 Mpx/s) el bus de PSRAM del P4 no sostiene
    // el drenado del ISP en tiempo real: confirmado en hardware como
    // "ISP: fifo overflow" y frames/s muy por debajo de lo esperado (~9-10 en
    // vez de 45), reproducible incluso con OCR y el monitor de desarrollo
    // desactivados — descartando ambos como causa. El salto de resolución del
    // commit b83183e (800×640 RAW8 → 1280×960 RAW10) es el origen real.
    //
    // fps = PCLK / (HTS × VTS). El sensor entrega PCLK=88.333.333 Hz y
    // HTS=1796 fijos para este modo (mismo timing de línea/pixel clock,
    // mismo binning) — la única perilla disponible sin tocar la ventana de
    // captura es VTS (líneas de blanking vertical): agrandarlo estira el
    // tiempo entre frames sin cambiar el ancho de banda instantáneo durante
    // las líneas activas.
    //   VTS original = 1093 (0x0445) → 45.0 fps
    //   VTS nuevo    = 1640 (0x0668) → 30.0 fps (pixel-rate ≈ 36.9 Mpx/s, -33%)
    // Registros 0x380e/0x380f (VTS hi/lo) — los mismos que usa la tabla de
    // init del driver — se sobreescriben acá después de set_format() porque
    // no hay una entrada de formato distinta para pedir esto vía API.
    //
    // PRIMER EXPERIMENTO, sin validar aún en hardware: si el fifo overflow
    // persiste a 30fps, bajar VTS_TARGET_FPS más (probar ~20-24fps) antes de
    // descartar esta vía.
    // -------------------------------------------------------------------------
    {
        const uint32_t PCLK_HZ  = 88333333;
        const uint16_t HTS      = 1796;
        const uint32_t VTS_TARGET_FPS = 30;
        uint16_t vts = (uint16_t)((PCLK_HZ + (HTS * VTS_TARGET_FPS) / 2) / (HTS * VTS_TARGET_FPS));
        ESP_ERROR_CHECK(esp_sccb_transmit_reg_a16v8(s_sccb_io, 0x380e, (uint8_t)(vts >> 8)));
        ESP_ERROR_CHECK(esp_sccb_transmit_reg_a16v8(s_sccb_io, 0x380f, (uint8_t)(vts & 0xFF)));
        ESP_LOGI(TAG, "VTS override: 1093 -> %u líneas (fps real objetivo: %"PRIu32", era 45)",
                 vts, VTS_TARGET_FPS);
    }

    // set_format ya aplicó el AE target por defecto del driver (0x50) —
    // lo subimos acá a AE_TARGET para forzar más exposición/ganancia (ver
    // comentario del #define más arriba).
    {
        int ae_target = AE_TARGET;
        ESP_ERROR_CHECK(esp_cam_sensor_set_para_value(sensor, ESP_CAM_SENSOR_EXPOSURE_VAL,
                                                       &ae_target, sizeof(ae_target)));
    }

    // -------------------------------------------------------------------------
    // 4. Controlador MIPI CSI-2
    //    h_res / v_res y lane_bit_rate_mbps salen del formato seleccionado
    //    (selected->mipi_info.mipi_clk, en Hz) en vez de una constante — cada
    //    modo del OV5647 tiene su propio IDI clock (ver comentario de
    //    parámetros de captura más arriba).
    // -------------------------------------------------------------------------
    const esp_cam_ctlr_csi_config_t csi_cfg = {
        .ctlr_id                = 0,
        .clk_src                = MIPI_CSI_PHY_CLK_SRC_DEFAULT,
        .h_res                  = selected->width,
        .v_res                  = selected->height,
        .data_lane_num          = selected->mipi_info.lane_num,
        .lane_bit_rate_mbps     = (uint32_t)((selected->mipi_info.mipi_clk + 500000) / 1000000),
        // RAW10 in/out — igual que en el modo RAW8 anterior, la conversión
        // Bayer→RGB real la hace el ISP (ver isp_cfg + esp_isp_demosaic_enable()
        // más abajo), no el controlador CSI. Confirmado que RAW8/RAW8 (mismo
        // patrón) funciona en hardware; RAW10/RAW10 sigue el mismo esquema.
        .input_data_color_type  = CAM_CTLR_COLOR_RAW10,
        .output_data_color_type = CAM_CTLR_COLOR_RAW10,
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
    // 5. Semáforo de frame y doble buffer de captura en PSRAM
    //    El semáforo sincroniza el ISR (csi_trans_finished_cb) con vision_task.
    //    Los buffers deben asignarse antes de registrar los callbacks porque
    //    csi_get_new_trans_cb los referencia desde el primer frame.
    //    1280×960 RGB565 = 2.457.600 bytes por buffer — mismo total de PSRAM
    //    que antes (2 × FRAME_SZ), solo que ahora ambos cumplen el rol de
    //    captura+display en vez de un buffer de captura + una copia aparte
    //    (ver comentario de s_frame_buf más arriba).
    // -------------------------------------------------------------------------
    s_frame_sem = xSemaphoreCreateBinary();
    if (!s_frame_sem) {
        ESP_LOGE(TAG, "failed to create frame semaphore");
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < 2; i++) {
        s_frame_buf[i] = heap_caps_malloc(FRAME_SZ, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
        if (!s_frame_buf[i]) {
            ESP_LOGW(TAG, "PSRAM no disponible para frame_buf[%d], intentando SRAM interna (%d bytes)", i, FRAME_SZ);
            s_frame_buf[i] = heap_caps_malloc(FRAME_SZ, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        }
        if (!s_frame_buf[i]) {
            ESP_LOGE(TAG, "sin memoria para frame_buf[%d] (%d bytes) — "
                          "habilitar PSRAM en menuconfig", i, FRAME_SZ);
            return ESP_ERR_NO_MEM;
        }
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
    // los datos pasan sin procesamiento (RAW10 → RAW10 directo).
    //
    // Referencia: esp_video_csi_device.c → start_isp() y
    //             examples/peripherals/camera/mipi_isp_dsi/main/mipi_isp_dsi_main.c
    // -------------------------------------------------------------------------
    isp_proc_handle_t isp_proc = NULL;
    {
        // output_data_color_type = RGB565: le dice al ISP que la salida es
        // color, pero el bloque de demosaico (interpolación Bayer→RGB) tiene
        // su propia máquina de estados independiente y hay que habilitarlo
        // explícitamente con esp_isp_demosaic_enable() — confirmado en
        // hardware: dos ejemplos oficiales de Espressif que NO lo llaman
        // (esp_driver_cam/test_apps/csi/main/test_csi_ov5647.c,
        // examples/peripherals/camera/mipi_isp_dsi/main/mipi_isp_dsi_main.c)
        // resultaron insuficientes — sin este call la imagen sigue en gris.
        // bayer_order = GBRG (no RGGB): confirmado en hardware que RGGB —
        // copiado de los modos RAW8 anteriores, donde sí funcionaba — produce
        // color corrido en este modo de binning (rojo se lee verde, beige sale
        // rojizo). GBRG es lo que declara ov5647_isp_info[4] en ov5647.c para
        // este formato exacto (MIPI_2lane_24Minput_RAW10_1280x960_binning), y
        // es consistente con el mismo desfase de fila que ya obligó a agregar
        // el espejado vertical de este modo (ver mirror_rgb565() en
        // monitor.c): el binning 2x2 reordena el barrido de píxeles en un eje,
        // lo que corre el patrón Bayer una fila además de invertir filas.
        // has_line_start_packet = false: a diferencia de los modos RAW8 y del
        // RAW10 1920x1080 (que dejan el bit LINE_SYNC_ENABLE de 0x4800 en 1,
        // ej. {0x4800, 0x34}), la tabla de registros del binning 1280x960 lo
        // deja en 0 ({0x4800, 0x24} en ov5647_settings.h — bit4 limpio). El
        // sensor en este modo específico no emite paquetes cortos de
        // line-start; si el ISP los espera igual (has_line_start_packet=true,
        // como se dejó al copiar la config de los modos anteriores) pierde la
        // sincronía de límites de línea de forma intermitente, lo que explica
        // los "ISP: data type error" en ráfagas (no en el 100% de los frames)
        // y el reinicio observado en hardware.
        const esp_isp_processor_cfg_t isp_cfg = {
            .clk_hz              = 80 * 1000 * 1000,
            .input_data_source   = ISP_INPUT_DATA_SOURCE_CSI,
            .input_data_color_type  = ISP_COLOR_RAW10,
            .output_data_color_type = ISP_COLOR_RGB565,
            .has_line_start_packet  = false,
            .has_line_end_packet    = false,
            .h_res       = FRAME_W,
            .v_res       = FRAME_H,
            .bayer_order = COLOR_RAW_ELEMENT_ORDER_GBRG,
        };
        ret = esp_isp_new_processor(&isp_cfg, &isp_proc);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ISP init failed: %s", esp_err_to_name(ret));
            return ret;
        }

        // NOTA sobre el glitch periódico de sincronía de línea (~cada 27-29s,
        // ver comentario de has_line_start_packet arriba): dispara
        // ISP_LL_EVENT_DATA_TYPE_ERR / ASYNC_FIFO_OVF / BUF_FULL en el ISP.
        //
        // Se probó deshabilitar la interrupción de esos eventos a nivel de
        // registro (isp_ll_enable_intr(..., ISP_LL_EVENT_ERROR_MASK, false))
        // para evitar el aluvión de ESP_EARLY_LOGE que causaba el watchdog de
        // interrupciones — pero eso también evita que se limpie el flag de
        // estado en el hardware (isp_hal_check_clear_intr_event, al inicio
        // del ISR, se ejecuta siempre, pero nunca corre si la interrupción
        // está enmascarada). Confirmado en hardware: la cámara se congeló por
        // completo la primera vez que ocurrió el glitch ("vision: frame
        // timeout" indefinido) — peor que el crash original. Revertido.
        //
        // El fix real está en el propio ESP-IDF instalado en esta máquina
        // (C:\esp\v6.0.1\esp-idf\components\esp_driver_isp\src\isp_core.c,
        // no en managed_components/ de este proyecto) — se sacaron ahí los
        // ESP_EARLY_LOGE bloqueantes de la rama de error, dejando la
        // limpieza del registro intacta. Ver comentario en ese archivo.
        // Ese parche vive fuera del repositorio del proyecto — si se
        // reinstala o actualiza ESP-IDF v6.0.1, hay que reaplicarlo (ver
        // [[vision-pipeline-esp32p4-v1x]] en la memoria del proyecto).

        ESP_ERROR_CHECK(esp_isp_enable(isp_proc));
        ESP_ERROR_CHECK(esp_isp_demosaic_enable(isp_proc));
        ESP_LOGI(TAG, "ISP enabled — demosaico RGB565 activo (RAW10 Bayer → RGB565)");
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
    // Pineada al core 1 junto con ocr_task — separada a propósito del core 0
    // (mic_task/lidar_task, audio y UART en tiempo real). Ver nota de pinning
    // en components/mic/mic.c.
    xTaskCreatePinnedToCore(vision_task, "vision", 4096, NULL, 4, NULL, 1);

    ESP_LOGI(TAG, "initialized — %ux%u RGB565 @%ufps, step=%d, edge_thr=%d",
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

// Ambas funciones de copia leen directo del buffer ping-pong (ver comentario
// de s_frame_buf más arriba) en vez de una copia aparte — el índice "listo"
// se captura una sola vez al entrar para no leer a mitad de camino un cambio
// de s_ready_idx hecho por el ISR.
bool vision_copy_display_frame(uint8_t *dst, size_t len)
{
    int idx = s_ready_idx;
    if (!dst || len < FRAME_SZ || idx < 0 || !s_ready) return false;
    memcpy(dst, s_frame_buf[idx], FRAME_SZ);
    return true;
}

// Scratch de una fila completa (RAM interna) para el submuestreo por columnas.
// s_frame_buf[] vive en PSRAM: leerlo con stride (un píxel sí, uno no) directo
// desde ahí rompe el acceso en ráfaga y satura el bus de PSRAM, compitiendo
// con el DMA del ISP que está recibiendo píxeles del sensor en tiempo real
// (visto en hardware: "ISP: fifo overflow" bajo esa carga). Por eso cada fila
// se trae completa con un memcpy secuencial a esta fila de scratch, y el
// descarte de columnas ocurre ahí (RAM interna, sin costo de bus) antes de
// escribir — también secuencial — al destino.
static uint16_t s_scale_row_scratch[FRAME_W];

bool vision_copy_display_frame_scaled(uint8_t *dst, size_t len, int scale)
{
    int idx = s_ready_idx;
    if (!dst || scale < 1 || idx < 0 || !s_ready) return false;
    if (FRAME_W % scale != 0 || FRAME_H % scale != 0) return false;
    int out_w = FRAME_W / scale;
    int out_h = FRAME_H / scale;
    size_t need = (size_t)out_w * out_h * 2;
    if (len < need) return false;

    const uint8_t *src_frame = s_frame_buf[idx];
    uint16_t *dst16 = (uint16_t *)dst;
    for (int y = 0; y < out_h; y++) {
        const uint16_t *src_row = (const uint16_t *)(src_frame + (size_t)(y * scale) * FRAME_W * 2);
        memcpy(s_scale_row_scratch, src_row, (size_t)FRAME_W * 2);  // PSRAM->SRAM, secuencial
        uint16_t *dst_row = dst16 + (size_t)y * out_w;
        for (int x = 0; x < out_w; x++) {
            dst_row[x] = s_scale_row_scratch[x * scale];  // SRAM->PSRAM, dst secuencial
        }
    }
    return true;
}

void vision_set_analysis_paused(bool paused)
{
    s_analysis_paused = paused;
}
