# Firmware del co-procesador ESP32-C6

Proyecto ESP-IDF **separado** del principal (target `esp32c6`, no
`esp32p4`). El C6 de esta placa (Waveshare ESP32-P4-Module-DEV-KIT) no tiene
pines de programación expuestos — no se puede flashear por USB directo. Se
sube via OTA sobre el mismo enlace SDIO, usando `components/cp_ota` del
proyecto principal.

## Por qué existe esto

El C6 vino de fábrica con un firmware genérico que reporta versión `0.0.0`
al ESP32-P4 — no es un release real de `esp_hosted`. Esto se confirmó en el
log de arranque del P4 (`eh_init_evt: major version mismatch — OTA
coprocessor from host`) y es sospechoso de varios problemas encontrados en
hardware real el 2026-09-18 (WiFi que a veces no sube, cámara sin frames
tras activar el reset del C6). Este proyecto compila el firmware correcto.

## 1. Compilar

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
cd coprocessor
idf.py set-target esp32c6
idf.py build
```

**Requisito previo (una sola vez por máquina):** el driver SDIO esclavo de
ESP-IDF v6.0.1 tiene un límite de 4092 bytes que rompe el build de este
proyecto. `esp_hosted` trae su propio parche (mismo patrón que
`tools/fix_esp_idf_isp_crash.bat` del proyecto principal, pero para este
bug distinto):

```powershell
python coprocessor/managed_components/espressif__esp_hosted/tools/eh.py patch-idf --idf-path C:\esp\v6.0.1\esp-idf
```

Igual que el parche de ISP, este vive en la instalación de ESP-IDF de la
máquina, no en este repo — hay que reaplicarlo si se reinstala ESP-IDF en
otra máquina.

## 2. Copiar el binario a la partición `slave_fw` del proyecto principal

El build genera `coprocessor/build/sentis_coprocessor.bin`. El proyecto
principal (carpeta raíz) tiene una partición `slave_fw` de 2 MB en
`partitions.csv` (offset `0x610000`) reservada exactamente para este
binario — `components/cp_ota` lo lee de ahí y lo empuja al C6 por OTA en
cada boot (no hace nada si la versión ya coincide).

Con el ESP32-P4 conectado y **sin nada usando el puerto serie** (cerrar
`idf.py monitor` si está abierto):

```powershell
. "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"
cd ..
esptool.py write_flash 0x610000 coprocessor/build/sentis_coprocessor.bin
```

## 3. Verificar

Reiniciar el P4 (`idf.py monitor`, o desconectar/reconectar) y revisar el
log: `cp_ota` debería reportar que actualizó el C6, y unos segundos después
`eh_init_evt` en el siguiente boot ya no debería mostrar "major version
mismatch" — la versión del co-procesador debería coincidir con
`host=3.0.7` (o la que tenga `components/wifi/idf_component.yml` en ese
momento).
