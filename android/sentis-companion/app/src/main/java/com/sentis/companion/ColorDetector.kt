package com.sentis.companion

import android.graphics.Bitmap
import android.graphics.Color
import java.util.concurrent.Executors

// =============================================================================
// ColorDetector — calcula el color dominante de un frame por análisis de
// píxeles en HSV, sin ningún modelo de IA (ver decisión del usuario
// 2026-09-22: sin API keys, sin planes de IA pagos, sin HTTP externo — y
// Gemini Nano no corre en el hardware de prueba, un Moto G20 sin AICore).
// Responde al mismo MSG_COLOR_REQUEST/MSG_COLOR_RESULT que arma
// components/ocr::ocr_detect_color() del lado del firmware, con el mismo
// espíritu que OcrTextRecognizer: un solo pedido/respuesta, no un loop.
//
// Corre en su propio executor de un solo hilo por el mismo motivo que
// OcrTextRecognizer: si el análisis corriera en el hilo que lee el socket
// (LinkClient.readLoop), el firmware no podría mandar el siguiente mensaje
// hasta que termine.
// =============================================================================

// Tamaño al que se reduce el frame antes de analizarlo — no hace falta
// resolución completa para estimar el color dominante, y reducir primero
// evita iterar ~1.2M píxeles por pedido.
private const val SAMPLE_SIZE = 32

// El frame que manda el ESP32 llega subexpuesto incluso con buena luz
// ambiente (reportado en hardware real 2026-09-22). Sin corregir esto, la
// mayoría de los píxeles caen bajo el umbral de "negro" de bucketFor() sin
// importar el color real del objeto. TARGET_BRIGHTNESS es el valor (HSV) al
// que se estira la MEDIANA de brillo del frame; MAX_GAIN limita cuánto se
// amplifica un frame realmente muy oscuro, para no inventar color de puro
// ruido de sensor.
//
// La mediana (no el percentil 95, como en un primer intento) es la
// referencia correcta acá: con el percentil 95, un solo reflejo o punto de
// luz chico en la esquina del cuadro (5% de los píxeles) alcanza para que el
// algoritmo crea que el frame ya está bien expuesto (ganancia ~1x) mientras
// la mayoría de los píxeles — el objeto que en realidad se está mirando —
// sigue oscuro. Confirmado en hardware real 2026-09-22: apuntando a la cara
// roja de un cubo Rubik, el log mostraba "ganancia aplicada: 1.1x" (casi sin
// corrección) y el resultado igual daba "negro". La mediana representa el
// brillo típico de la escena, así que no se deja engañar por ese 5% brillante.
private const val TARGET_BRIGHTNESS = 0.5f
private const val MAX_GAIN = 10f

private enum class ColorBucket(val label: String) {
    NEGRO("negro"),
    BLANCO("blanco"),
    GRIS("gris"),
    ROJO("rojo"),
    NARANJA("naranja"),
    AMARILLO("amarillo"),
    VERDE("verde"),
    CELESTE("celeste"),
    AZUL("azul"),
    VIOLETA("violeta"),
    ROSA("rosa"),
}

class ColorDetector(
    private val onLog: (String) -> Unit,
) {
    private val executor = Executors.newSingleThreadExecutor { r -> Thread(r, "color-detector") }

    // onFrame entrega el bitmap ya decodificado (mismo criterio que
    // OcrTextRecognizer.recognize), para que la UI pueda mostrar el frame que
    // la cámara del ESP32 capturó.
    fun detect(jpeg: ByteArray, onFrame: (Bitmap) -> Unit, onResult: (String) -> Unit) {
        executor.execute {
            val bitmap = decodeSentisFrame(jpeg)
            if (bitmap == null) {
                onLog("Color: no se pudo decodificar el JPEG (${jpeg.size} bytes)")
                return@execute
            }
            onFrame(bitmap)
            val colorName = dominantColorName(bitmap)
            onResult(colorName)
        }
    }

    private fun dominantColorName(bitmap: Bitmap): String {
        val sample = Bitmap.createScaledBitmap(bitmap, SAMPLE_SIZE, SAMPLE_SIZE, true)
        val pixels = IntArray(sample.width * sample.height)
        sample.getPixels(pixels, 0, sample.width, 0, 0, sample.width, sample.height)
        if (sample !== bitmap) sample.recycle()

        val gain = exposureGain(pixels)
        onLog("Color: ganancia de exposición aplicada: ${"%.1f".format(gain)}x")

        val counts = IntArray(ColorBucket.entries.size)
        val hsv = FloatArray(3)
        for (pixel in pixels) {
            Color.colorToHSV(applyGain(pixel, gain), hsv)
            counts[bucketFor(hsv).ordinal]++
        }

        val topBucket = ColorBucket.entries[counts.indices.maxBy { counts[it] }]
        onLog("Color dominante: ${topBucket.label}")
        return topBucket.label
    }

    // Estima cuánto estirar el brillo con la mediana del frame (ver comentario
    // de TARGET_BRIGHTNESS/MAX_GAIN más arriba) y aplica la misma ganancia a
    // los tres canales por igual, así que no altera el matiz/saturación que ve
    // colorToHSV, solo el valor.
    private fun exposureGain(pixels: IntArray): Float {
        val hsv = FloatArray(3)
        val values = FloatArray(pixels.size)
        for (i in pixels.indices) {
            Color.colorToHSV(pixels[i], hsv)
            values[i] = hsv[2]
        }
        values.sort()
        val median = values[values.size / 2]
        // Frame casi sin señal (lente tapado, oscuridad total): igual conviene
        // intentar con la ganancia máxima antes que rendirse a "negro" directo
        // — si es puro ruido de sensor, el resultado va a ser inestable entre
        // capturas, pero al menos no descarta de entrada un objeto real que
        // esté genuinamente muy oscuro.
        if (median <= 0.01f) return MAX_GAIN
        return (TARGET_BRIGHTNESS / median).coerceIn(1f, MAX_GAIN)
    }

    private fun applyGain(pixel: Int, gain: Float): Int {
        if (gain <= 1f) return pixel
        val r = (Color.red(pixel) * gain).toInt().coerceIn(0, 255)
        val g = (Color.green(pixel) * gain).toInt().coerceIn(0, 255)
        val b = (Color.blue(pixel) * gain).toInt().coerceIn(0, 255)
        return Color.rgb(r, g, b)
    }

    // hsv[0] = hue (0-360), hsv[1] = saturación (0-1), hsv[2] = valor (0-1).
    // Los cortes de matiz siguen el círculo cromático estándar (rojo en 0°/360°,
    // verde en 120°, azul en 240°) — no hay nada específico de SENTIS acá.
    private fun bucketFor(hsv: FloatArray): ColorBucket {
        val (h, s, v) = hsv
        if (v < 0.15f) return ColorBucket.NEGRO
        if (s < 0.15f) return if (v > 0.85f) ColorBucket.BLANCO else ColorBucket.GRIS
        return when {
            h < 15f || h >= 345f -> ColorBucket.ROJO
            h < 45f -> ColorBucket.NARANJA
            h < 70f -> ColorBucket.AMARILLO
            h < 170f -> ColorBucket.VERDE
            h < 200f -> ColorBucket.CELESTE
            h < 260f -> ColorBucket.AZUL
            h < 320f -> ColorBucket.VIOLETA
            else -> ColorBucket.ROSA
        }
    }

    fun close() {
        executor.shutdown()
    }
}
