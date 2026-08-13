# setup_ocr.ps1 — Prepara los modelos pp_ocr_v6 para lectura OCR en SENTIS.
#
# Ejecutar desde la raiz del proyecto DESPUES de haber compilado (o corrido
# "idf.py reconfigure") al menos una vez, para que el IDF Component Manager
# haya descargado el paquete espressif/pp_ocr_v6 (declarado en
# components/ocr/idf_component.yml) a managed_components/:
#   powershell -ExecutionPolicy Bypass -File scripts/setup_ocr.ps1
#
# El paquete espressif/pp_ocr_v6 trae los .espdl YA COMPILADOS para ESP32-P4
# en su propia carpeta models/p4/ — no hay descarga adicional que hacer, solo
# copiar esa carpeta tal cual a sdcard_files/ (mismo nombre de subcarpeta que
# usa el componente por default vía Kconfig, CONFIG_PP_OCR_V6_MODEL_SDCARD_DIR
# = "models/p4" — ver sdkconfig.defaults).
#
# Resultado:
#   sdcard_files/models/p4/pp_ocr_v6_det_s8.espdl
#   sdcard_files/models/p4/pp_ocr_v6_rec_s8.espdl
#   sdcard_files/models/p4/pp_ocr_v6_rec_s16.espdl
#   sdcard_files/models/p4/pp_ocr_v6_rec_s16_w640.espdl
#
# ocr.cpp solo usa det_s8 + rec_s16 (los defaults de pp_ocr_v6::Det/Rec) —
# los otros dos se copian igual porque ya vienen en el paquete, por si se
# quiere experimentar con otra variante más adelante.

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PROJ_ROOT = Split-Path -Parent $PSScriptRoot
$SRC_DIR   = "$PROJ_ROOT\managed_components\espressif__pp_ocr_v6\models\p4"
$DATA_DST  = "$PROJ_ROOT\sdcard_files\models\p4"

Write-Host ""
Write-Host "=== Configurando modelos OCR (pp_ocr_v6) para SENTIS ===" -ForegroundColor Cyan
Write-Host ""

if (-not (Test-Path $SRC_DIR)) {
    Write-Host "ERROR: no se encontro $SRC_DIR" -ForegroundColor Red
    Write-Host "Correr primero (desde la raiz del proyecto, con el entorno IDF activado):"
    Write-Host "  idf.py reconfigure"
    Write-Host "para que el Component Manager descargue espressif/pp_ocr_v6."
    exit 1
}

$espdl_files = Get-ChildItem -Path $SRC_DIR -Filter "*.espdl"
if (-not $espdl_files) {
    Write-Host "ERROR: no se encontraron archivos .espdl en $SRC_DIR" -ForegroundColor Red
    exit 1
}

Write-Host "[1/1] Copiando $($espdl_files.Count) archivo(s) .espdl -> $DATA_DST ..."
New-Item -ItemType Directory -Force $DATA_DST | Out-Null
Copy-Item -Path $espdl_files.FullName -Destination $DATA_DST -Force
Write-Host "      OK"

Write-Host ""
Write-Host "=== Setup completo ===" -ForegroundColor Green
Write-Host ""
Write-Host "Proximos pasos:"
Write-Host "  1. Copiar sdcard_files\models\ a la microSD (/sdcard/models/)"
Write-Host "  2. Compilar:  idf.py build"
Write-Host "  3. Flashear:  idf.py flash monitor"
Write-Host ""
