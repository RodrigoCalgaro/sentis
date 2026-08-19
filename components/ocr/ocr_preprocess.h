#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ocr_preprocess — conversión de RGB565 (salida real del demosaico ISP) a
// RGB888 para el detector/reconocedor de texto de pp_ocr_v6.
//
// El ISP del ESP32-P4 demosaica el Bayer RGGB del OV5647 a RGB565 antes de
// que el frame llegue a vision_copy_display_frame() (ver components/vision/
// vision.c) — color real por píxel, ya no una aproximación por promediado de
// bloques Bayer. pp_ocr_v6 (pp_ocr_v6_image_preprocessor.cpp: sample_channel())
// asume siempre 3 bytes/píxel en dl::image::img_t, así que igual hace falta
// desempacar a RGB888 — pero ya no hace falta downsampling: el detector
// reescala internamente a su tamaño de entrada fijo (736x736), así que
// conviene aprovechar la resolución completa.
//
// La cámara está montada rotada respecto a la vista del usuario — se aplica
// la misma corrección de orientación ya validada en hardware (rotar 90°CW +
// espejar horizontal, equivalente a una transposición fila↔columna; ver
// tools/monitor_viewer.py para la versión de referencia en Python).
// =============================================================================

// Convierte un frame RGB565 de w×h píxeles (src) a RGB888 intercalado (dst),
// con la corrección de orientación aplicada (transposición). La salida queda
// con ancho y alto INTERCAMBIADOS respecto a la entrada: ancho_salida=h,
// alto_salida=w. dst debe tener capacidad para w*h*3 bytes.
void ocr_preprocess_rgb565_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst);

#ifdef __cplusplus
}
#endif
