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
// Corrección de orientación: en el modo RAW10 binning 1280x960 (ver
// components/vision/vision.c) comparar capturas reales contra la orientación
// esperada mostró que hace falta un espejado vertical (arriba-abajo) — a
// diferencia de los modos RAW8 anteriores, que necesitaban transposición
// (rotar 90°CW + espejar horizontal). Mismos bits de mirror/flip del sensor
// (0x3820/0x3821) en ambos casos; el binning 2x2 del OV5647 es conocido por
// invertir el orden de barrido de píxeles respecto al modo sin binning, lo
// que explica la diferencia. Si se vuelve a cambiar de modo de captura,
// volver a verificar con una foto real (ver tools/monitor_viewer.py) antes de
// asumir que esta corrección sigue siendo la correcta.
// =============================================================================

// Convierte un frame RGB565 de w×h píxeles (src) a RGB888 intercalado (dst),
// con la corrección de orientación aplicada (espejado vertical). La salida
// mantiene el mismo ancho y alto que la entrada. dst debe tener capacidad
// para w*h*3 bytes.
void ocr_preprocess_rgb565_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst);

#ifdef __cplusplus
}
#endif
