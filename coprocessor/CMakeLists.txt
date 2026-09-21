# SPDX-License-Identifier: Apache-2.0
#
# Firmware del ESP32-C6 a bordo (co-procesador WiFi del ESP32-P4, via SDIO).
# Proyecto ESP-IDF SEPARADO del principal (target esp32c6, no esp32p4) — ver
# README.md en esta carpeta para como compilarlo y subirlo.

cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sentis_coprocessor)
