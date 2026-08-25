#include "ocr_preprocess.h"

static inline void unpack_rgb565(uint16_t px, uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = (uint8_t)(((px >> 11) & 0x1F) << 3);
    *g = (uint8_t)(((px >> 5)  & 0x3F) << 2);
    *b = (uint8_t)(( px        & 0x1F) << 3);
}

void ocr_preprocess_rgb565_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst)
{
    const uint16_t *src16 = (const uint16_t *)src;

    // Espejado vertical (arriba-abajo), mismas dimensiones que la entrada
    // (ver nota de orientación en el .h).
    for (int y = 0; y < h; y++) {
        const uint16_t *row = src16 + (size_t)(h - 1 - y) * w;
        uint8_t *dst_row = dst + (size_t)y * w * 3;

        for (int x = 0; x < w; x++) {
            uint8_t r, g, b;
            unpack_rgb565(row[x], &r, &g, &b);

            uint8_t *px = dst_row + (size_t)x * 3;
            px[0] = r;
            px[1] = g;
            px[2] = b;
        }
    }
}
