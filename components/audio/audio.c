#include "audio.h"
#include "bus.h"
#include "board_config.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

static const char *TAG = "audio";

// Convierte porcentaje (0-100) al byte de registro ES8311 REG32.
// Fórmula del driver oficial espressif/es8311 v1.0.0:
//   reg32 = (vol% × 256 / 100) − 1   para vol > 0
//   reg32 = 0x00                      para vol = 0 (silencio)
// Ejemplos:  0%=0x00  50%=0x7F  70%=0xB2  80%=0xCB  100%=0xFF
#define VOL_TO_REG(pct) \
    (((pct) <= 0) ? 0 : (((pct) >= 100) ? 0xFF : (uint8_t)(((pct) * 256 / 100) - 1)))

// =============================================================================
// Fase 2: Audio — codec ES8311 + amplificador NS4150B
//
// Topología:
//   ESP32-P4 I2S0 ──→ ES8311 (DAC) ──→ NS4150B (PA 3W) ──→ parlante
//
// Orden de inicialización (crítico — igual al ejemplo Waveshare 12_I2SCodec):
//   1. I2S0 PRIMERO: MCLK/BCLK/LRCK deben estar activos ANTES del codec init.
//      El ES8311 necesita ver las señales de clock para sincronizar su PLL.
//   2. ES8311: registros escritos con la secuencia exacta del componente oficial
//      espressif/es8311 v1.0.0 (extraída directamente del source).
//   3. NS4150B: GPIO53 HIGH = amplificador encendido.
//
// Registros ES8311 — fuente autoritativa:
//   managed_components/espressif__es8311/es8311.c (descargado via component mgr).
//   La función es8311_init() + es8311_sample_frequency_config() determina los
//   valores correctos para MCLK=4096000Hz, Fs=16000Hz, 16-bit, I2S estándar.
//
// Bugs que causaban silencio en versiones anteriores:
//   REG01=0x30 → 0x3F: habilita TODOS los bloques de clock (era parcial)
//   REG06=0x00 → 0x03: BCLK divider /4 (era incorrecto)
//   REG0A=0x00 → 0x0C: DAC 16-bit (era 24-bit default → mismatch con I2S)
//   REG13=0x06 → 0x10: habilita HP output drive (sin esto: silencio total)
//   REG1C=0x44 → 0x6A: ADC EQ bypass (valor correcto del componente)
// =============================================================================

static i2s_chan_handle_t        s_tx     = NULL;
static i2c_master_dev_handle_t  s_es8311 = NULL;
static bool                     s_initialized = false;

#define WAV_READ_BUF_SIZE  4096
static uint8_t  s_wav_buf[WAV_READ_BUF_SIZE];
static int16_t  s_stereo_buf[WAV_READ_BUF_SIZE];

// -----------------------------------------------------------------------------
// es8311_write / es8311_read — acceso a registros del codec.
// -----------------------------------------------------------------------------
static esp_err_t es8311_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_es8311, buf, 2, -1);
}

static esp_err_t es8311_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_es8311, &reg, 1, val, 1, -1);
}

// -----------------------------------------------------------------------------
// wav_parse_header — parsea encabezado RIFF/WAV.
// Deja el cursor al inicio de los datos PCM.
// -----------------------------------------------------------------------------
typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
} wav_info_t;

static esp_err_t wav_parse_header(FILE *f, wav_info_t *out)
{
    char id[4];
    uint32_t size;

    if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) != 0) return ESP_ERR_INVALID_ARG;
    fread(&size, 4, 1, f);
    if (fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4) != 0) return ESP_ERR_INVALID_ARG;

    bool found_fmt = false, found_data = false;
    while (!feof(f) && !(found_fmt && found_data)) {
        if (fread(id, 1, 4, f) != 4) break;
        if (fread(&size, 4, 1, f) != 1) break;
        if (memcmp(id, "fmt ", 4) == 0) {
            uint16_t fmt, ch, bps, align;
            uint32_t sr, br;
            fread(&fmt, 2, 1, f);  fread(&ch, 2, 1, f);
            fread(&sr, 4, 1, f);   fread(&br, 4, 1, f);
            fread(&align, 2, 1, f); fread(&bps, 2, 1, f);
            if (fmt != 1) return ESP_ERR_NOT_SUPPORTED;
            if (size > 16) fseek(f, (long)(size - 16), SEEK_CUR);
            out->sample_rate = sr; out->channels = ch; out->bits_per_sample = bps;
            found_fmt = true;
        } else if (memcmp(id, "data", 4) == 0) {
            out->data_size = size; found_data = true;
        } else {
            fseek(f, (long)size, SEEK_CUR);
        }
    }
    return (found_fmt && found_data) ? ESP_OK : ESP_ERR_NOT_FOUND;
}

// =============================================================================
// audio_init
// =============================================================================
esp_err_t audio_init(void)
{
    esp_err_t ret;

    // -------------------------------------------------------------------------
    // 1. I2S0 PRIMERO — MCLK/BCLK/LRCK activos antes de inicializar el codec.
    //    El ES8311 sincroniza su PLL al MCLK externo durante es8311_init().
    //    Si el codec se configura antes de que I2S esté corriendo, el PLL
    //    no puede hacer lock y no hay señal de audio.
    // -------------------------------------------------------------------------
    i2s_chan_config_t chan_cfg =
        I2S_CHANNEL_DEFAULT_CONFIG(BOARD_I2S_NUM, I2S_ROLE_MASTER);
    ret = i2s_new_channel(&chan_cfg, &s_tx, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    const i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = AUDIO_SAMPLE_RATE,
            .clk_src        = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple  = I2S_MCLK_MULTIPLE_256,   // MCLK = 256×Fs = 4.096 MHz
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BOARD_I2S_MCLK_GPIO,    // GPIO13
            .bclk = BOARD_I2S_SCLK_GPIO,    // GPIO12
            .ws   = BOARD_I2S_LRCK_GPIO,    // GPIO10
            .dout = BOARD_I2S_DSDIN_GPIO,   // GPIO9  → ES8311 data in
            .din  = BOARD_I2S_ASDOUT_GPIO,  // GPIO11 ← ES8311 data out
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ret = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_LOGI(TAG, "I2S%d running — MCLK=GPIO%d %uHz×256=%uHz",
             BOARD_I2S_NUM, BOARD_I2S_MCLK_GPIO,
             AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 256);

    // -------------------------------------------------------------------------
    // 2. ES8311 — bus I2C compartido (nueva API i2c_master.h)
    // -------------------------------------------------------------------------
    ret = bus_i2c_init();
    if (ret != ESP_OK) return ret;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BOARD_CODEC_I2C_ADDR,  // 0x18
        .scl_speed_hz    = 100000,
    };
    ret = i2c_master_bus_add_device(bus_get_i2c(), &dev_cfg, &s_es8311);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 I2C add device failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // -------------------------------------------------------------------------
    // 3. Secuencia de registros ES8311 — extraída de espressif/es8311 v1.0.0
    //    es8311_init() + es8311_sample_frequency_config() + es8311_fmt_config()
    //    para MCLK=4096000 Hz, Fs=16000 Hz, 16-bit, I2S estándar, slave mode.
    //
    //    Coeficiente de clock (tabla coeff_div del componente):
    //    {4096000, 16000, pre_div=1, pre_multi=0, adc_div=1, dac_div=1,
    //     fs_mode=0, lrck_h=0x00, lrck_l=0xFF, bclk_div=4, adc_osr=0x10, dac_osr=0x10}
    // -------------------------------------------------------------------------

    // Reset completo
    if (es8311_write(0x00, 0x1F) != ESP_OK) {
        ESP_LOGE(TAG, "ES8311 not responding — check I2C (SDA=GPIO%d SCL=GPIO%d addr=0x%02X)",
                 BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO, BOARD_CODEC_I2C_ADDR);
        return ESP_FAIL;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
    ESP_ERROR_CHECK(es8311_write(0x00, 0x00));  // clear reset
    ESP_ERROR_CHECK(es8311_write(0x00, 0x80));  // power on

    // Clock: MCLK from pin, habilitar TODOS los bloques de clock (0x3F)
    // REG01[7]: 0=MCLK pin, 1=BCLK pin. [6]: MCLK invert. [5:0]: 0x3F=all clocks on.
    ESP_ERROR_CHECK(es8311_write(0x01, 0x3F));

    // Clock dividers para MCLK=4096000Hz, Fs=16000Hz:
    // REG02: (pre_div-1)<<5 | pre_multi<<3 = (0<<5)|(0<<3) = 0x00
    ESP_ERROR_CHECK(es8311_write(0x02, 0x00));
    // REG03: (fs_mode<<6) | adc_osr = (0<<6)|0x10 = 0x10
    ESP_ERROR_CHECK(es8311_write(0x03, 0x10));
    // REG04: dac_osr = 0x10
    ESP_ERROR_CHECK(es8311_write(0x04, 0x10));
    // REG05: (adc_div-1)<<4 | (dac_div-1) = 0x00
    ESP_ERROR_CHECK(es8311_write(0x05, 0x00));
    // REG06: bclk_div=4 → bclk_div-1=3 (bclk_div < 19)
    ESP_ERROR_CHECK(es8311_write(0x06, 0x03));
    // REG07: lrck_h = 0x00
    ESP_ERROR_CHECK(es8311_write(0x07, 0x00));
    // REG08: lrck_l = 0xFF
    ESP_ERROR_CHECK(es8311_write(0x08, 0xFF));

    // Formato I2S: estándar (Philips), slave, 16-bit
    // REG09 (SDP In - ADC):  bits[3:2]=0b11 → 16-bit → 0x0C
    ESP_ERROR_CHECK(es8311_write(0x09, 0x0C));
    // REG0A (SDP Out - DAC): bits[3:2]=0b11 → 16-bit → 0x0C
    // (CRÍTICO: 0x00 = 24-bit default → mismatch con I2S de 16-bit → silencio)
    ESP_ERROR_CHECK(es8311_write(0x0A, 0x0C));

    // Power up del sistema analógico
    ESP_ERROR_CHECK(es8311_write(0x0D, 0x01));  // power up analog circuitry
    ESP_ERROR_CHECK(es8311_write(0x0E, 0x02));  // enable analog PGA + ADC modulator
    ESP_ERROR_CHECK(es8311_write(0x12, 0x00));  // power up DAC
    // REG13=0x10: enable output to HP drive — SIN ESTO EL PARLANTE NO SUENA
    ESP_ERROR_CHECK(es8311_write(0x13, 0x10));

    // EQ bypass
    ESP_ERROR_CHECK(es8311_write(0x1C, 0x6A));  // ADC EQ bypass + DC offset cancel
    ESP_ERROR_CHECK(es8311_write(0x37, 0x08));  // DAC EQ bypass

    // Volumen DAC desde AUDIO_VOLUME_PERCENT (definido en audio.h)
    ESP_ERROR_CHECK(es8311_write(0x32, VOL_TO_REG(AUDIO_VOLUME_PERCENT)));
    ESP_LOGI(TAG, "DAC volume %d%% → REG32=0x%02X",
             AUDIO_VOLUME_PERCENT, VOL_TO_REG(AUDIO_VOLUME_PERCENT));

    // Micrófono analógico (habilitado para futuras fases, sin efecto en playback)
    ESP_ERROR_CHECK(es8311_write(0x14, 0x1A));  // enable analog MIC
    ESP_ERROR_CHECK(es8311_write(0x17, 0xC8));  // ADC gain

    // -------------------------------------------------------------------------
    // UNMUTE DAC — REG31 bits[6:5] = DAC_L_MUTE / DAC_R_MUTE
    //
    // El flujo esp_codec_dev_open() que usa el BSP de Waveshare para ESP32-P4
    // llama internamente a es8311_voice_mute(false), que limpia estos bits.
    // Nuestro init directo de registros nunca tocaba REG31.
    // Si el valor default post-reset tiene bits[6:5] seteados → silencio total
    // aunque todos los demás registros estén correctos.
    // -------------------------------------------------------------------------
    {
        uint8_t r31 = 0;
        es8311_read(0x31, &r31);
        ESP_LOGI(TAG, "REG31 pre-unmute=0x%02X (mute bits[6:5]=0x%02X)",
                 r31, (r31 >> 5) & 0x03);
        r31 &= ~(BIT(6) | BIT(5));  // clear DAC_L_MUTE + DAC_R_MUTE
        ESP_ERROR_CHECK(es8311_write(0x31, r31));
        ESP_LOGI(TAG, "REG31 unmuted → 0x%02X", r31);
    }

    // ---- Diagnóstico: leer de vuelta registros clave ----
    {
        uint8_t r00, r01, r13, r31, r32;
        es8311_read(0x00, &r00);
        es8311_read(0x01, &r01);
        es8311_read(0x13, &r13);
        es8311_read(0x31, &r31);
        es8311_read(0x32, &r32);
        ESP_LOGI(TAG, "ES8311 regs — REG00=0x%02X REG01=0x%02X "
                       "REG13=0x%02X(exp 0x10) REG31=0x%02X REG32=0x%02X(exp 0xFF)",
                 r00, r01, r13, r31, r32);
    }

    ESP_LOGI(TAG, "ES8311 configured — MCLK=%uHz Fs=%uHz 16-bit vol=100%%",
             AUDIO_SAMPLE_RATE * 256, AUDIO_SAMPLE_RATE);

    // -------------------------------------------------------------------------
    // 4. Amplificador NS4150B — PA_CTRL_GPIO HIGH = encendido
    // -------------------------------------------------------------------------
    const gpio_config_t pa_cfg = {
        .pin_bit_mask = 1ULL << BOARD_PA_CTRL_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(BOARD_PA_CTRL_GPIO, 1);
    // Delay para que NS4150B estabilice su alimentación interna antes del audio
    vTaskDelay(pdMS_TO_TICKS(100));

    s_initialized = true;
    ESP_LOGI(TAG, "initialized — I2S%d %uHz 16-bit stereo, PA GPIO%d ON",
             BOARD_I2S_NUM, AUDIO_SAMPLE_RATE, BOARD_PA_CTRL_GPIO);

    // -------------------------------------------------------------------------
    // 5. Tono de diagnóstico — cuadrado 440 Hz × 400 ms
    //    Si se escucha: el chain ES8311 → NS4150B → parlante funciona.
    //    Remover esta llamada una vez confirmado el funcionamiento.
    // -------------------------------------------------------------------------
    audio_test_tone();

    return ESP_OK;
}

// =============================================================================
// audio_set_volume — ajusta el volumen en runtime sin reiniciar el audio.
//
// vol_percent: 0 (silencio) a 100 (máximo).
// Escribe el registro ES8311 REG32 usando la misma fórmula del driver oficial.
// =============================================================================
esp_err_t audio_set_volume(int vol_percent)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (vol_percent < 0)   vol_percent = 0;
    if (vol_percent > 100) vol_percent = 100;

    uint8_t reg = VOL_TO_REG(vol_percent);
    esp_err_t ret = es8311_write(0x32, reg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "volume %d%% → REG32=0x%02X", vol_percent, reg);
    }
    return ret;
}

// =============================================================================
// audio_test_tone — onda cuadrada 440 Hz × 400 ms sin necesitar SD.
// =============================================================================
void audio_test_tone(void)
{
    if (!s_initialized) return;

    const int    HALF = 18;    // 16000/(2×440) ≈ 18 muestras por semiperíodo
    const int16_t AMP = 12000; // 73% de escala completa

    int16_t buf[72];  // 36 muestras estéreo = 1 período
    for (int i = 0; i < 36; i++) {
        int16_t v    = (i < HALF) ? AMP : -AMP;
        buf[i*2]     = v;
        buf[i*2 + 1] = v;
    }

    size_t written;
    ESP_LOGI(TAG, "test tone 440 Hz × 400 ms");
    // 400 ms × 16000 Hz = 6400 muestras = 6400/36 ≈ 178 períodos
    for (int rep = 0; rep < 178; rep++) {
        i2s_channel_write(s_tx, buf, sizeof(buf), &written, pdMS_TO_TICKS(50));
    }
}

// =============================================================================
// audio_play_wav — reproduce WAV 16-bit PCM desde el VFS.
// Formato: AUDIO_SAMPLE_RATE Hz, 16-bit signed, mono o estéreo.
// Mono se duplica automáticamente a estéreo. Bloqueante.
// =============================================================================
esp_err_t audio_play_wav(const char *path)
{
    if (!s_initialized) {
        ESP_LOGE(TAG, "audio_init() not called");
        return ESP_ERR_INVALID_STATE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    wav_info_t info = {0};
    esp_err_t ret = wav_parse_header(f, &info);
    if (ret != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "WAV header error: %s", esp_err_to_name(ret));
        return ret;
    }

    if (info.bits_per_sample != 16) {
        fclose(f);
        ESP_LOGE(TAG, "%s: only 16-bit PCM (%u-bit found)", path, info.bits_per_sample);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (info.sample_rate != AUDIO_SAMPLE_RATE) {
        fclose(f);
        ESP_LOGE(TAG, "%s: %"PRIu32" Hz ≠ %d Hz", path, info.sample_rate, AUDIO_SAMPLE_RATE);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "playing %s — %"PRIu32" Hz 16-bit %s (%"PRIu32" bytes)",
             path, info.sample_rate,
             info.channels == 1 ? "mono" : "stereo", info.data_size);

    uint32_t remaining = info.data_size;
    while (remaining > 0) {
        size_t to_read = remaining < WAV_READ_BUF_SIZE ? (size_t)remaining : WAV_READ_BUF_SIZE;
        size_t n = fread(s_wav_buf, 1, to_read, f);
        if (n == 0) break;
        remaining -= (uint32_t)n;

        size_t bytes_written;
        if (info.channels == 2) {
            i2s_channel_write(s_tx, s_wav_buf, n, &bytes_written, pdMS_TO_TICKS(2000));
        } else {
            int16_t *src  = (int16_t *)s_wav_buf;
            int      samps = (int)(n / 2);
            for (int i = 0; i < samps; i++) {
                s_stereo_buf[i*2]     = src[i];
                s_stereo_buf[i*2 + 1] = src[i];
            }
            i2s_channel_write(s_tx, s_stereo_buf, (size_t)(samps * 4),
                              &bytes_written, pdMS_TO_TICKS(2000));
        }
    }

    fclose(f);
    ESP_LOGI(TAG, "playback done: %s", path);
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// audio_mute — habilita o deshabilita el amplificador NS4150B.
// -----------------------------------------------------------------------------
void audio_mute(bool mute)
{
    if (!s_initialized) return;
    gpio_set_level(BOARD_PA_CTRL_GPIO, mute ? 0 : 1);
}
