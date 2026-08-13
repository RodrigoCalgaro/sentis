#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// =============================================================================
// ocr_preprocess — conversión de Bayer crudo a RGB888 para el detector/
// reconocedor de texto de pp_ocr_v6.
//
// La cámara OV5647 entrega RAW8 Bayer (patrón RGGB) sin demosaico — el ISP del
// ESP32-P4 está configurado en modo pass-through (ver components/vision/vision.c).
// pp_ocr_v6 (pp_ocr_v6_image_preprocessor.cpp: sample_channel()) asume siempre
// 3 bytes por píxel en dl::image::img_t sin importar pix_type, así que no
// alcanza con pasar un buffer de un solo canal — hace falta un buffer RGB888
// real, aunque sea una aproximación.
//
// Aproximación usada (riesgo de precisión documentado en el plan — validar con
// hardware real): cada bloque Bayer 2x2 (R,G,G,B) se promedia a un solo valor
// de luminancia, que se replica en los 3 canales de salida. Esto da además un
// downsample natural a mitad de resolución (800x640 -> 400x320), útil porque
// el detector igual reescala internamente a su tamaño de entrada fijo.
// =============================================================================

// Convierte un frame Bayer RGGB de w×h bytes (src) a una imagen RGB888
// intercalada (dst), con downsample 2x2 (R=G=B en cada píxel — no es un
// debayer real) y corrección de orientación (rotar 90° + espejar horizontal,
// ver comentario en ocr_preprocess.c).
//
// IMPORTANTE: la salida queda con ancho y alto INTERCAMBIADOS respecto a la
// entrada por la corrección de orientación: ancho_salida=h/2, alto_salida=w/2.
// w y h deben ser pares. dst debe tener capacidad para (w/2)*(h/2)*3 bytes.
void ocr_preprocess_bayer_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst);

#ifdef __cplusplus
}
#endif
