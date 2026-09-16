<#
.SYNOPSIS
    Parchea la instalacion local de ESP-IDF v6.0.1 para evitar un crash
    conocido en el driver del ISP del ESP32-P4.

.DESCRIPTION
    El ISR de eventos de error del ISP (esp_driver_isp/src/isp_core.c)
    imprime por consola con ESP_EARLY_LOGE -- un print BLOQUEANTE, caracter
    por caracter -- por cada evento de error, DENTRO del ISR.

    En el modo de captura que usa SENTIS (sensor OV5647, RAW10 binning
    1280x960) hay un glitch periodico de sincronia de linea MIPI (cada
    ~27-29s, ver el comentario de has_line_start_packet en
    components/vision/vision.c) que dispara esos eventos de error en
    rafaga -- cientos de veces en el mismo milisegundo. Con el codigo
    original de ESP-IDF, esos cientos de prints bloqueantes seguidos
    monopolizan la CPU el tiempo suficiente para disparar el watchdog de
    interrupciones y reiniciar el equipo (panic "Interrupt wdt timeout on
    CPU0").

    Este script reemplaza esos prints por un simple contador (sin I/O). La
    limpieza del registro de estado del hardware (isp_hal_check_clear_intr_
    event, unas lineas mas arriba en la misma funcion) no se toca -- eso es
    lo que evita que el pipeline de captura se trabe.

    IMPORTANTE: este parche vive en la instalacion de ESP-IDF (fuera del
    repositorio del proyecto). Hay que volver a correr este script cada vez
    que se instala o reinstala ESP-IDF v6.0.1 en una maquina nueva, o si se
    actualiza/reinstala la version existente. No hace falta despues de un
    simple "git pull" del proyecto -- el parche no vive en el repo.

    Se investigo primero si una version mas nueva de ESP-IDF ya resuelve
    esto: no es el caso (confirmado contra la rama principal del repo
    oficial de espressif/esp-idf) -- el mismo codigo sin throttling sigue
    presente ahi.

.NOTES
    Correr via fix_esp_idf_isp_crash.bat (doble clic), o directamente:
      powershell -ExecutionPolicy Bypass -File fix_esp_idf_isp_crash.ps1

    Requiere que IDF_PATH este definida en el entorno (por ejemplo, corriendo
    esto desde una terminal "ESP-IDF 6.0.1 PowerShell" ya activada), o que
    idf.py este disponible en el PATH.
#>

$ErrorActionPreference = 'Stop'

function Write-Info($msg)  { Write-Host $msg -ForegroundColor Cyan }
function Write-Ok($msg)    { Write-Host $msg -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host $msg -ForegroundColor Yellow }
function Write-Err2($msg)  { Write-Host $msg -ForegroundColor Red }

Write-Info "=== Fix ISP crash - ESP-IDF v6.0.1 (proyecto SENTIS) ==="
Write-Host ""

# -----------------------------------------------------------------------
# 1. Resolver IDF_PATH
# -----------------------------------------------------------------------
$idfPath = $env:IDF_PATH

if (-not $idfPath) {
    $idfCmd = Get-Command idf.py -ErrorAction SilentlyContinue
    if ($idfCmd) {
        # idf.py vive en <IDF_PATH>\tools\idf.py
        $toolsDir = Split-Path $idfCmd.Source -Parent
        $idfPath = Split-Path $toolsDir -Parent
    }
}

if (-not $idfPath -or -not (Test-Path $idfPath)) {
    Write-Err2 "No se pudo encontrar la instalacion de ESP-IDF (IDF_PATH no definida)."
    Write-Warn2 "Corre este script desde una terminal con el entorno de ESP-IDF activo,"
    Write-Warn2 "por ejemplo activando primero el perfil del instalador EIM:"
    Write-Warn2 '  . "C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1"'
    Write-Warn2 "y volviendo a correr este .bat desde esa misma terminal."
    exit 1
}

Write-Info "IDF_PATH detectado: $idfPath"

$targetFile = Join-Path $idfPath "components\esp_driver_isp\src\isp_core.c"
if (-not (Test-Path $targetFile)) {
    Write-Err2 "No se encontro isp_core.c en:"
    Write-Err2 "  $targetFile"
    Write-Warn2 "Verifica que IDF_PATH apunte a una instalacion de ESP-IDF v6.0.1."
    exit 1
}

# -----------------------------------------------------------------------
# 2. Leer el archivo y detectar si ya esta parcheado
# -----------------------------------------------------------------------
$original = Get-Content -Raw -LiteralPath $targetFile
$usesCRLF = $original.Contains("`r`n")
$normalized = $original -replace "`r`n", "`n"

$marker = "s_sentis_isp_error_count"
if ($normalized.Contains($marker)) {
    Write-Ok "Ya esta parcheado (se encontro '$marker' en isp_core.c)."
    Write-Ok "No hay nada que hacer."
    exit 0
}

# -----------------------------------------------------------------------
# 3. Buscar el bloque original a reemplazar
# -----------------------------------------------------------------------
$oldBlock = @(
    '    if ((error_events & ISP_LL_EVENT_DATA_TYPE_ERR) || (error_events & ISP_LL_EVENT_DATA_TYPE_SETTING_ERR)) {',
    '        ESP_EARLY_LOGE(TAG, "data type error");',
    '    }',
    '    if ((error_events & ISP_LL_EVENT_ASYNC_FIFO_OVF) || (error_events & ISP_LL_EVENT_BUF_FULL)) {',
    '        ESP_EARLY_LOGE(TAG, "fifo overflow");',
    '    }',
    '    if ((error_events & ISP_LL_EVENT_HVNUM_SETTING_ERR) || (error_events & ISP_LL_EVENT_MIPI_HNUM_UNMATCH)) {',
    '        ESP_EARLY_LOGE(TAG, "hnum / vnum setting error");',
    '    }',
    '    if (error_events & ISP_LL_EVENT_GAMMA_XCOORD_ERR) {',
    '        ESP_EARLY_LOGE(TAG, "gamma xcoord error");',
    '    }',
    '    if (error_events & ISP_LL_EVENT_CROP_ERR) {',
    '        ESP_EARLY_LOGE(TAG, "crop error");',
    '    }'
) -join "`n"

if (-not $normalized.Contains($oldBlock)) {
    Write-Err2 "No se encontro el bloque esperado dentro de isp_core.c."
    Write-Warn2 "Puede que esta instalacion de ESP-IDF tenga una version distinta de"
    Write-Warn2 "este archivo (patch level / build distinto de v6.0.1). Aplicar el"
    Write-Warn2 "parche a mano -- ver README.md, seccion 'Prerequisitos'."
    exit 1
}

# -----------------------------------------------------------------------
# 4. Construir el bloque parcheado
# -----------------------------------------------------------------------
$newBlock = @(
    '    // ---------------------------------------------------------------------',
    '    // PARCHE LOCAL -- proyecto SENTIS (arduino_sentis).',
    '    //',
    '    // Los ESP_EARLY_LOGE de aca abajo son prints BLOQUEANTES (caracter por',
    '    // caracter, esp_rom_printf) ejecutados DENTRO de este ISR. En hardware',
    '    // ESP32-P4 rev v1.3 con el OV5647 en modo RAW10 binning 1280x960, un',
    '    // glitch periodico de sincronia de linea MIPI (cada ~27-29s, ver',
    '    // sentis/components/vision/vision.c, comentario de has_line_start_packet)',
    '    // dispara error_events en rafaga -- cientos de veces en el mismo',
    '    // milisegundo. Con el log original, esos cientos de prints bloqueantes',
    '    // seguidos monopolizan la CPU el tiempo suficiente para disparar el',
    '    // watchdog de interrupciones y reiniciar el equipo (panic "Interrupt wdt',
    '    // timeout on CPU0", visto en hardware, atascado en uart_rx_readbuff).',
    '    //',
    '    // isp_hal_check_clear_intr_event() ya limpio el registro de estado mas',
    '    // arriba (linea ~284) antes de esta rama -- eso es lo que importa para que',
    '    // el pipeline del ISP no se trabe; loguear o no loguear el evento no',
    '    // afecta esa limpieza.',
    '    //',
    '    // Fix: reemplazar los prints bloqueantes por contadores livianos',
    '    // (incremento de un entero, sin I/O). El evento se sigue limpiando y',
    '    // contando; solo se saca el costo de imprimir por consola dentro del ISR.',
    '    //',
    '    // ESTE ARCHIVO VIVE FUERA DEL REPOSITORIO DEL PROYECTO (es parte de la',
    '    // instalacion de ESP-IDF en esta maquina, no de managed_components/). Si',
    '    // se reinstala o actualiza ESP-IDF, o se corre en otra maquina/CI, este',
    '    // parche NO viaja solo -- correr tools/fix_esp_idf_isp_crash.bat del',
    '    // proyecto SENTIS de nuevo.',
    '    // ---------------------------------------------------------------------',
    '    static volatile uint32_t s_sentis_isp_error_count = 0;',
    '    if (error_events) {',
    '        s_sentis_isp_error_count++;',
    '    }'
) -join "`n"

# -----------------------------------------------------------------------
# 5. Backup + escritura
# -----------------------------------------------------------------------
$backupFile = "$targetFile.pre-sentis-patch.bak"
if (-not (Test-Path $backupFile)) {
    Copy-Item -LiteralPath $targetFile -Destination $backupFile
    Write-Info "Backup del original guardado en:"
    Write-Info "  $backupFile"
}

$patchedNormalized = $normalized.Replace($oldBlock, $newBlock)

if ($usesCRLF) {
    $patchedFinal = $patchedNormalized -replace "`n", "`r`n"
} else {
    $patchedFinal = $patchedNormalized
}

Set-Content -LiteralPath $targetFile -Value $patchedFinal -NoNewline -Encoding UTF8

Write-Host ""
Write-Ok "Parche aplicado correctamente en:"
Write-Ok "  $targetFile"
Write-Host ""
Write-Ok "Ya podes compilar el proyecto normalmente (idf.py build)."
exit 0
