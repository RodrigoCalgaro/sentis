#!/usr/bin/env python3
"""
SENTIS Monitor Viewer
Recibe frames JPEG + zona detectada por USB CDC y los muestra en pantalla.

Instalacion (una sola vez):
    pip install pyserial opencv-python numpy

Uso:
    python tools/monitor_viewer.py COM3          (Windows)
    python tools/monitor_viewer.py /dev/ttyACM1  (Linux)

El puerto es el mismo COM que usa idf.py monitor (USB Serial/JTAG del ESP32-P4).
idf.py monitor y este script NO pueden estar abiertos al mismo tiempo.

Protocolo de framing:
    [4]  magic: AB CD EF 01
    [4]  tamano JPEG en bytes (uint32, little-endian)
    [1]  zona: 0=NONE  1=LEFT  2=CENTER  3=RIGHT
    [1]  reservado
    [N]  datos JPEG (escala de grises, patron Bayer visible)
"""

import sys
import struct
import numpy as np
import cv2
import serial

MAGIC      = bytes([0xAB, 0xCD, 0xEF, 0x01])
HDR_SIZE   = 10
MAX_BUF    = 2 * 1024 * 1024   # descartar si el buffer supera 2 MB (sin sincronizacion)
MAX_JPEG   = 500_000           # sanidad: rechazar frames > 500 KB

ZONE_LABEL = {0: "NONE", 1: "LEFT", 2: "CENTER", 3: "RIGHT"}
ZONE_COLOR = {0: (160, 160, 160), 1: (40, 120, 220), 2: (30, 130, 230), 3: (40, 180, 60)}
#            NONE=gris           LEFT=azul            CENTER=naranja       RIGHT=verde


def extract_frame(buf: bytearray):
    """
    Busca el primer frame valido en buf.
    Retorna (jpeg_bytes, zone_int, buf_restante) si encontro un frame completo.
    Retorna (None, None, buf_recortado) si necesita mas datos.
    """
    while True:
        idx = buf.find(MAGIC)
        if idx < 0:
            return None, None, bytearray(buf[-3:])   # conservar posible magic parcial

        if len(buf) - idx < HDR_SIZE:
            return None, None, bytearray(buf[idx:])  # esperar cabecera completa

        size = struct.unpack_from('<I', buf, idx + 4)[0]
        zone = buf[idx + 8]

        if size == 0 or size > MAX_JPEG:
            buf = buf[idx + 4:]                       # magic falso, buscar el siguiente
            continue

        end = idx + HDR_SIZE + size
        if len(buf) < end:
            return None, None, bytearray(buf[idx:])  # esperar datos completos

        jpeg = bytes(buf[idx + HDR_SIZE:end])
        return jpeg, zone, bytearray(buf[end:])


def overlay(frame_bgr, zone: int) -> None:
    """Dibuja la barra de zona sobre el frame (in-place)."""
    label = ZONE_LABEL.get(zone, "?")
    color = ZONE_COLOR.get(zone, (200, 200, 200))
    h, w = frame_bgr.shape[:2]
    cv2.rectangle(frame_bgr, (0, 0), (w, 50), (25, 25, 25), -1)
    cv2.putText(frame_bgr, f"SENTIS  zona: {label}",
                (12, 36), cv2.FONT_HERSHEY_SIMPLEX, 1.1, color, 2, cv2.LINE_AA)


def main(port: str) -> None:
    print(f"Conectando a {port} ...")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as e:
        print(f"Error: {e}")
        sys.exit(1)

    print("Conectado. Esperando frames del ESP32-P4... (ESC para salir)")
    buf = bytearray()
    frames_rx = 0

    with ser:
        while True:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk
                print(f"  rx {len(chunk)} bytes  buf={len(buf)}", end="\r")

            # Evitar crecimiento ilimitado si no hay sincronizacion
            if len(buf) > MAX_BUF:
                print("[warn] buffer overflow — resincronizando")
                buf.clear()
                continue

            jpeg, zone, buf = extract_frame(buf)
            if jpeg is None:
                continue

            img = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_GRAYSCALE)
            if img is None:
                continue

            # Corregir orientación del sensor: rotar 90° CW + flip horizontal
            img = cv2.rotate(img, cv2.ROTATE_90_CLOCKWISE)
            img = cv2.flip(img, 1)

            # Descartar buffer acumulado para mostrar siempre el frame más reciente
            buf = bytearray()

            frames_rx += 1
            display = cv2.cvtColor(img, cv2.COLOR_GRAY2BGR)
            overlay(display, zone)
            cv2.imshow("SENTIS Monitor", display)

            if frames_rx % 30 == 0:
                print(f"  {frames_rx} frames recibidos  |  ultimo: {len(jpeg)} bytes  zona: {ZONE_LABEL.get(zone,'?')}")

            if cv2.waitKey(1) == 27:   # ESC
                break

    cv2.destroyAllWindows()
    print(f"Cerrado. Total frames: {frames_rx}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1])
