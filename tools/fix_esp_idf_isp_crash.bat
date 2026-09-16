@echo off
REM Parchea la instalacion local de ESP-IDF v6.0.1 para evitar un crash
REM conocido del driver del ISP (ver README.md, seccion "Prerequisitos",
REM y tools\fix_esp_idf_isp_crash.ps1 para el detalle completo).
REM
REM Correr esto UNA VEZ por maquina/instalacion de ESP-IDF, antes de
REM compilar el proyecto por primera vez (o despues de reinstalar/
REM actualizar ESP-IDF v6.0.1). Necesita el entorno de ESP-IDF activo
REM (IDF_PATH definida) o idf.py en el PATH.

setlocal
set "SCRIPT_DIR=%~dp0"

powershell -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%fix_esp_idf_isp_crash.ps1"
set "RESULT=%ERRORLEVEL%"

echo.
if "%RESULT%"=="0" (
    echo Listo.
) else (
    echo Hubo un problema aplicando el parche -- revisa los mensajes de arriba.
)
pause
exit /b %RESULT%
