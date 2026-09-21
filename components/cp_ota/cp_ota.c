#include "cp_ota.h"

#include <inttypes.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_hosted.h"
#include "esp_hosted_api_types.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "cp_ota";

#define CP_OTA_CHUNK_SIZE 1500
#define CP_OTA_PARTITION_LABEL "slave_fw"

// Calcula el tamano real de la imagen guardada en la particion (header +
// segmentos + checksum + hash opcional) y extrae el string de version del
// app_desc del primer segmento — misma logica que usa el ejemplo oficial de
// Espressif (examples/ota/coprocessor_ota/.../ota_partition.c).
static esp_err_t parse_image(const esp_partition_t *part, size_t *out_size,
                              char *out_version, size_t out_version_len)
{
    esp_image_header_t header;
    esp_err_t ret = esp_partition_read(part, 0, &header, sizeof(header));
    if (ret != ESP_OK) {
        return ret;
    }
    if (header.magic != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGW(TAG, "particion '%s' sin firmware valido (magic incorrecto) — nada para actualizar",
                 part->label);
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = sizeof(header);
    size_t total = sizeof(header);
    for (int i = 0; i < header.segment_count; i++) {
        esp_image_segment_header_t seg;
        ret = esp_partition_read(part, offset, &seg, sizeof(seg));
        if (ret != ESP_OK) {
            return ret;
        }
        if (i == 0) {
            esp_app_desc_t app_desc;
            size_t app_desc_offset = offset + sizeof(seg);
            if (esp_partition_read(part, app_desc_offset, &app_desc, sizeof(app_desc)) == ESP_OK) {
                strlcpy(out_version, app_desc.version, out_version_len);
            } else {
                strlcpy(out_version, "desconocida", out_version_len);
            }
        }
        total += sizeof(seg) + seg.data_len;
        offset += sizeof(seg) + seg.data_len;
    }

    size_t padding = (16 - (total % 16)) % 16;
    total += padding + 1;  // relleno de alineacion + 1 byte de checksum
    if (header.hash_appended) {
        size_t hash_padding = (16 - (total % 16)) % 16;
        total += hash_padding + 32;  // SHA256
    }

    *out_size = total;
    return ESP_OK;
}

esp_err_t cp_ota_check_and_update(void)
{
    esp_hosted_coprocessor_fwver_t cur = {0};
    bool have_cur_version = (esp_hosted_get_coprocessor_fwversion(&cur) == ESP_OK);
    if (have_cur_version) {
        ESP_LOGI(TAG, "version actual del co-procesador: %" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 cur.major1, cur.minor1, cur.patch1);
    } else {
        ESP_LOGW(TAG, "no se pudo leer la version actual del co-procesador — sigo igual");
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CP_OTA_PARTITION_LABEL);
    if (!part) {
        ESP_LOGW(TAG, "particion '%s' no existe — nada para actualizar", CP_OTA_PARTITION_LABEL);
        return ESP_ERR_NOT_FOUND;
    }

    size_t fw_size = 0;
    char new_version[32] = {0};
    esp_err_t ret = parse_image(part, &fw_size, new_version, sizeof(new_version));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "particion '%s' vacia o sin provisionar — nada para actualizar (ver coprocessor/README.md)",
                 CP_OTA_PARTITION_LABEL);
        return ret;
    }
    if (fw_size > part->size) {
        ESP_LOGE(TAG, "firmware (%u bytes) mas grande que la particion (%" PRIu32 " bytes)",
                 (unsigned)fw_size, part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    if (have_cur_version) {
        char cur_version[32];
        snprintf(cur_version, sizeof(cur_version), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                 cur.major1, cur.minor1, cur.patch1);
        if (strcmp(cur_version, new_version) == 0) {
            ESP_LOGI(TAG, "co-procesador ya tiene la version %s — no hace falta actualizar", cur_version);
            return ESP_OK;
        }
    }

    ESP_LOGW(TAG, "actualizando firmware del co-procesador de %s (%u bytes) via OTA sobre SDIO...",
             new_version, (unsigned)fw_size);

    ret = esp_hosted_slave_ota_begin();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    uint8_t chunk[CP_OTA_CHUNK_SIZE];
    size_t offset = 0;
    while (offset < fw_size) {
        size_t n = (fw_size - offset > CP_OTA_CHUNK_SIZE) ? CP_OTA_CHUNK_SIZE : (fw_size - offset);
        ret = esp_partition_read(part, offset, chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "error leyendo la particion en offset %u: %s", (unsigned)offset,
                     esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }
        ret = esp_hosted_slave_ota_write(chunk, n);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ota_write fallo en offset %u: %s", (unsigned)offset, esp_err_to_name(ret));
            esp_hosted_slave_ota_end();
            return ret;
        }
        offset += n;
    }

    ret = esp_hosted_slave_ota_end();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ota_end fallo: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGW(TAG, "firmware transferido — activando...");
    ret = esp_hosted_slave_ota_activate();
    if (ret != ESP_OK) {
        // Confirmado en hardware (2026-09-18): el C6 puede reiniciarse solo
        // apenas termina la transferencia, sin esperar esta RPC — en ese
        // caso este timeout es normal (el C6 ya no esta ahi para responder)
        // y la version SI queda actualizada. Se confirma en el proximo
        // boot via esp_hosted_get_coprocessor_fwversion() arriba.
        ESP_LOGW(TAG, "ota_activate sin respuesta (%s) — puede ser normal si el C6 ya se reinicio solo, "
                 "se confirma en el proximo boot", esp_err_to_name(ret));
        return ESP_OK;
    }

    ESP_LOGW(TAG, "co-procesador activando firmware nuevo — deberia reiniciar");
    return ESP_OK;
}
