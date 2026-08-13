#include "ocr_preprocess.h"

// El frame crudo llega con la orientación física del sensor, NO con la
// orientación en que el usuario ve el mundo a través de los lentes —
// tools/monitor_viewer.py corrige esto solo para mostrarlo en pantalla
// (cv2.rotate(ROTATE_90_CLOCKWISE) seguido de cv2.flip(1)). Un detector/
// reconocedor de texto no es invariante a rotación/espejo, así que sin esta
// misma corrección el modelo recibe el texto rotado y espejado — eso es lo
// que causaba las "alucinaciones" (caracteres chinos, texto sin sentido)
// aunque la imagen capturada en sí fuera perfectamente legible.
//
// rotate 90° CW + flip horizontal es algebraicamente equivalente a una
// transposición simple (intercambiar fila y columna): para una imagen de
// ancho W y alto H, rotar 90°CW da una de ancho H/alto W, y espejarla horiz.
// cancela el desplazamiento de la rotación dejando final(fila=c, col=r) =
// origen(fila=r, col=c) — pura transposición. Verificado a mano con un
// ejemplo de 2x3 antes de implementar esto.
void ocr_preprocess_bayer_to_rgb888(const uint8_t *src, int w, int h, uint8_t *dst)
{
    int gw = w / 2; // ancho del gris intermedio, antes de transponer
    int gh = h / 2; // alto del gris intermedio, antes de transponer

    // Salida ya transpuesta: ancho=gh, alto=gw.
    for (int y = 0; y < gh; y++) {
        const uint8_t *row0 = src + (size_t)(y * 2) * w;
        const uint8_t *row1 = row0 + w;

        for (int x = 0; x < gw; x++) {
            int x0 = x * 2;
            uint16_t sum = (uint16_t)row0[x0] + row0[x0 + 1] + row1[x0] + row1[x0 + 1];
            uint8_t gray = (uint8_t)(sum >> 2);

            // gris[y][x] (fila y, col x) -> salida[x][y] (fila x, col y)
            uint8_t *px = dst + ((size_t)x * gh + y) * 3;
            px[0] = gray;
            px[1] = gray;
            px[2] = gray;
        }
    }
}
