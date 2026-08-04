# setup_espeak.ps1 — Prepara eSpeak-NG para SENTIS (fuentes + datos de voz).
#
# Ejecutar desde la raiz del proyecto UNA SOLA VEZ antes del primer build:
#   powershell -ExecutionPolicy Bypass -File scripts/setup_espeak.ps1
#
# Resultado:
#   components/espeak-ng/src/libespeak-ng/  <- fuentes C de sintesis
#   components/espeak-ng/include/           <- headers publicos y ucd/ucd.h
#   sdcard_files/espeak-ng-data/            <- datos de voz compilados (copiar a SD)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$PROJ_ROOT = Split-Path -Parent $PSScriptRoot
$TEMP_BASE = "$env:TEMP\sentis_espeak_setup"

Write-Host ""
Write-Host "=== Configurando eSpeak-NG para SENTIS ===" -ForegroundColor Cyan
Write-Host ""

# =========================================================================
# FASE 1: Fuentes C (1.51.1 desde GitHub archive) + headers ucd-tools
# =========================================================================
$SRC_VERSION  = "1.51.1"
$SRC_TARBALL  = "$TEMP_BASE\src_$SRC_VERSION.tar.gz"
$SRC_EXTRACT  = "$TEMP_BASE\src"
$SRC_URL      = "https://github.com/espeak-ng/espeak-ng/archive/refs/tags/$SRC_VERSION.tar.gz"

$SRC_DST      = "$PROJ_ROOT\components\espeak-ng\src\libespeak-ng"
$INC_DST      = "$PROJ_ROOT\components\espeak-ng\include\espeak-ng"
$UCD_INC_DST  = "$PROJ_ROOT\components\espeak-ng\include\ucd"

New-Item -ItemType Directory -Force $TEMP_BASE | Out-Null

if (-not (Test-Path $SRC_TARBALL)) {
    Write-Host "[1/6] Descargando fuentes eSpeak-NG $SRC_VERSION ..."
    Invoke-WebRequest -Uri $SRC_URL -OutFile $SRC_TARBALL -UseBasicParsing
    Write-Host "      OK ($([math]::Round((Get-Item $SRC_TARBALL).Length/1MB,1)) MB)"
} else {
    Write-Host "[1/6] Tarball fuentes ya descargado."
}

if (-not (Test-Path $SRC_EXTRACT)) {
    Write-Host "[2/6] Extrayendo fuentes ..."
    New-Item -ItemType Directory -Force $SRC_EXTRACT | Out-Null
    tar -xzf $SRC_TARBALL -C $SRC_EXTRACT
    Write-Host "      OK"
} else {
    Write-Host "[2/6] Fuentes ya extraidas."
}

$EXTRACTED = Get-ChildItem $SRC_EXTRACT -Directory | Select-Object -First 1

Write-Host "[3/6] Copiando fuentes C de sintesis ..."
New-Item -ItemType Directory -Force $SRC_DST | Out-Null
$EXCLUDE = @("compiledata.c","compilembrola.c","compilelex.c","compiledictionary.c",
             "espeak.c","espeak-ng.c","mbrowrap.c","sPlayer.c")
# Fuentes libespeak-ng
Get-ChildItem "$($EXTRACTED.FullName)\src\libespeak-ng" -Filter "*.c" |
    Where-Object { $_.Name -notin $EXCLUDE } |
    Copy-Item -Destination $SRC_DST -Force
Get-ChildItem "$($EXTRACTED.FullName)\src\libespeak-ng" -Filter "*.h" |
    Copy-Item -Destination $SRC_DST -Force
# Fuentes ucd-tools (renombradas para evitar colisiones)
$UCD_SRC = "$($EXTRACTED.FullName)\src\ucd-tools\src"
foreach ($f in @("case.c","categories.c","ctype.c","proplist.c","scripts.c","tostring.c")) {
    Copy-Item "$UCD_SRC\$f" "$SRC_DST\ucd_$f" -Force
}
Write-Host "      OK ($((Get-ChildItem $SRC_DST -Filter '*.c').Count) archivos .c)"

# Parche: alinear tipo uint32_t en espeak_command.h
$cmd_h = "$SRC_DST\espeak_command.h"
if (Test-Path $cmd_h) {
    $content = Get-Content $cmd_h -Raw
    if ($content -match "int sync_espeak_terminated_msg\(unsigned int") {
        $content = $content -replace '#include <espeak-ng/espeak_ng.h>',
            "#include <espeak-ng/espeak_ng.h>`n#include <stdint.h>"
        $content = $content -replace 'int sync_espeak_terminated_msg\(unsigned int unique_identifier',
            'int sync_espeak_terminated_msg(uint32_t unique_identifier'
        Set-Content $cmd_h $content -NoNewline
        Write-Host "      Parche uint32_t aplicado en espeak_command.h"
    }
}

Write-Host "[4/6] Copiando headers publicos y ucd ..."
New-Item -ItemType Directory -Force $INC_DST | Out-Null
New-Item -ItemType Directory -Force $UCD_INC_DST | Out-Null
Copy-Item -Path "$($EXTRACTED.FullName)\src\include\espeak-ng\*" -Destination $INC_DST -Force
Copy-Item "$($EXTRACTED.FullName)\src\ucd-tools\src\include\ucd\ucd.h" "$UCD_INC_DST\ucd.h" -Force
Write-Host "      OK"

# =========================================================================
# FASE 2: Datos de voz compilados (1.51 Windows MSI)
# =========================================================================
$MSI_VERSION = "1.51"
$MSI_FILE    = "$TEMP_BASE\espeak-ng-X64.msi"
$MSI_EXTRACT = "$TEMP_BASE\msi_out"
$MSI_URL     = "https://github.com/espeak-ng/espeak-ng/releases/download/$MSI_VERSION/espeak-ng-X64.msi"
$DATA_DST    = "$PROJ_ROOT\sdcard_files\espeak-ng-data"

if (-not (Test-Path $MSI_FILE)) {
    Write-Host "[5/6] Descargando datos de voz compilados (MSI $MSI_VERSION) ..."
    Invoke-WebRequest -Uri $MSI_URL -OutFile $MSI_FILE -UseBasicParsing
    Write-Host "      OK ($([math]::Round((Get-Item $MSI_FILE).Length/1MB,1)) MB)"
} else {
    Write-Host "[5/6] MSI ya descargado."
}

Write-Host "[6/6] Extrayendo y copiando datos de voz → $DATA_DST ..."
New-Item -ItemType Directory -Force $MSI_EXTRACT | Out-Null
$p = Start-Process "msiexec.exe" -ArgumentList "/a `"$MSI_FILE`" /qn TARGETDIR=`"$MSI_EXTRACT`"" -Wait -PassThru
if ($p.ExitCode -ne 0) {
    Write-Host "      AVISO: msiexec salió con código $($p.ExitCode)" -ForegroundColor Yellow
}
New-Item -ItemType Directory -Force $DATA_DST | Out-Null
$msi_data = "$MSI_EXTRACT\eSpeak NG\espeak-ng-data"
if (Test-Path $msi_data) {
    Copy-Item -Path "$msi_data\*" -Destination $DATA_DST -Recurse -Force
    $file_count = (Get-ChildItem $DATA_DST -Recurse -File).Count
    Write-Host "      OK ($file_count archivos)"
} else {
    Write-Host "      ERROR: datos no encontrados en el MSI" -ForegroundColor Red
}

# Crear voice file para sintesis formant espanol (sin MBROLA)
$voice_other = "$DATA_DST\voices\other"
New-Item -ItemType Directory -Force $voice_other | Out-Null
Set-Content "$voice_other\es" "name Spanish`nlanguage es`ngender male`n" -NoNewline
Write-Host "      Voice file 'es' creado en voices/other/"

Write-Host ""
Write-Host "=== Setup completo ===" -ForegroundColor Green
Write-Host ""
Write-Host "Proximos pasos:"
Write-Host "  1. Copiar sdcard_files\espeak-ng-data\ a la microSD (/sdcard/espeak-ng-data/)"
Write-Host "  2. Compilar:  idf.py build"
Write-Host "  3. Flashear:  idf.py flash monitor"
Write-Host ""
