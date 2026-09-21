#include "storage.h"
#include "board_config.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"

static const char *TAG = "storage";

// =============================================================================
// Tarjeta microSD — SDMMC 4-bit onboard, Waveshare ESP32-P4-Module-DEV-KIT.
//
// IMPORTANTE — LDO interno #4 (SOC_SDMMC_IO_POWER_EXTERNAL en ESP32-P4):
//   El ESP32-P4 alimenta las IOs del bus SDMMC (CMD, CLK, D0-D3) a través de
//   un LDO interno dedicado, NO directamente desde el rail de 3.3V del sistema.
//   Sin inicializar este LDO, las líneas SDMMC no tienen tensión y la tarjeta
//   no puede responder → send_op_cond retorna ESP_ERR_TIMEOUT.
//   Este es el mismo patrón que el LDO #3 @ 2500 mV que habilita el PHY MIPI
//   CSI en vision.c.
//   Fuente: ejemplo oficial Waveshare 09_sdmmc, Kconfig.projbuild:
//     CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_INTERNAL_IO=y (default ESP32-P4)
//     CONFIG_EXAMPLE_SD_PWR_CTRL_LDO_IO_ID=4
//
// Pines (confirmados con el ejemplo oficial Waveshare 09_sdmmc):
//   CLK = GPIO43   CMD = GPIO44
//   D0  = GPIO39   D1  = GPIO40   D2  = GPIO41   D3  = GPIO42
//
// Requisito: FAT32. Si la tarjeta es > 32 GB usar fat32format o mkfs.fat -F 32.
// =============================================================================

static bool s_mounted = false;

#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO
// El controlador SDMMC en ESP32-P4/ESP-IDF v6.x solo se puede inicializar
// UNA vez — y esp_hosted (WiFi, ver components/wifi) ya lo hizo antes de que
// storage_init() corra (wifi_init() se llama primero en main/sentis.c).
// Si dejamos que esp_vfs_fat_sdmmc_mount() vuelva a llamar sdmmc_host_init(),
// falla con "no available sd host controller" aunque el WiFi haya subido
// bien — confirmado en hardware real (2026-09-18). Mismo workaround que usa
// el ejemplo oficial de Espressif para esta combinacion exacta
// (esp-hosted-mcu examples/mcu_hosted_sdio_sdmmc_combined/.../
// sd_card_functions.c, WORKAROUND_HOSTED_DOES_SDMMC_HOST_INIT): pasarle
// funciones init/deinit dummy al host, ya que el controlador compartido ya
// esta arriba.
static esp_err_t sdmmc_host_init_noop(void) { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_noop(void) { return ESP_OK; }
#endif

esp_err_t storage_init(void)
{
    esp_err_t ret;

    // -------------------------------------------------------------------------
    // 1. LDO interno #4 — alimentación de las IOs del slot SDMMC
    //    Sin este paso, CMD/CLK/D0-D3 no tienen tensión y el card no responde.
    // -------------------------------------------------------------------------
    sd_pwr_ctrl_ldo_config_t ldo_cfg = {
        .ldo_chan_id = BOARD_SD_LDO_CHAN_ID,
    };
    sd_pwr_ctrl_handle_t pwr_ctrl_handle = NULL;
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &pwr_ctrl_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD LDO#%d init failed: %s — check esp_hw_support",
                 BOARD_SD_LDO_CHAN_ID, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "SD LDO#%d enabled", BOARD_SD_LDO_CHAN_ID);

    // -------------------------------------------------------------------------
    // 2. Host SDMMC con handle de control de energía
    // -------------------------------------------------------------------------
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.pwr_ctrl_handle = pwr_ctrl_handle;
#if CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SDIO
    host.init = &sdmmc_host_init_noop;
    host.deinit = &sdmmc_host_deinit_noop;
#endif

    // -------------------------------------------------------------------------
    // 3. Slot 0 en modo 4-bit con los pines del board
    // -------------------------------------------------------------------------
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width  = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.clk    = BOARD_SD_CLK_GPIO;
    slot.cmd    = BOARD_SD_CMD_GPIO;
    slot.d0     = BOARD_SD_D0_GPIO;
    slot.d1     = BOARD_SD_D1_GPIO;
    slot.d2     = BOARD_SD_D2_GPIO;
    slot.d3     = BOARD_SD_D3_GPIO;

    const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,  // nunca formatear automáticamente
        .max_files              = 8,
        .allocation_unit_size   = 16 * 1024,
    };

    // -------------------------------------------------------------------------
    // 4. Montar filesystem FAT
    // -------------------------------------------------------------------------
    sdmmc_card_t *card = NULL;
    ret = esp_vfs_fat_sdmmc_mount(STORAGE_MOUNT_POINT, &host, &slot,
                                             &mount_cfg, &card);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "mount failed — check FAT32 format");
        } else {
            ESP_LOGE(TAG, "sdmmc init failed: %s — check card is inserted",
                     esp_err_to_name(ret));
        }
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    s_mounted = true;
    ESP_LOGI(TAG, "microSD mounted at %s", STORAGE_MOUNT_POINT);
    return ESP_OK;
}

bool storage_is_mounted(void)
{
    return s_mounted;
}
