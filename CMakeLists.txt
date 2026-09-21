# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# Workaround — espressif/esp_wifi_remote (componente wifi, requerido por
# esp_hosted) selecciona su Kconfig por version con
# `orsource "./Kconfig.idf_v$ESP_IDF_VERSION.in"`, pero NINGUNA parte de un
# build vanilla de ESP-IDF v6.0.1 exporta esa variable de entorno (se
# confirmó: no está en tools/kconfig_new/confgen.py, kconfiglib.py, ni
# tools/cmake/kconfig.cmake — el único similar es IDF_INIT_VERSION, que es
# otra cosa). Sin ella, ese orsource no encuentra archivo y no define nada,
# mientras que el esp_wifi.h inyectado por esp_hosted sigue referenciando
# esos símbolos igual → "undeclared" al compilar eh_host_wifi.c. Fijamos la
# variable nosotros mismos, mismo formato "major.minor" que el propio
# CMakeLists.txt del paquete ya arma desde IDF_VERSION_MAJOR/MINOR.
# Revisar si sigue haciendo falta al actualizar de ESP-IDF v6.0.1.
set(ENV{ESP_IDF_VERSION} "6.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sentis)
