#!/usr/bin/env python3
"""
SENTIS Monitor Viewer
Recibe frames JPEG + zona detectada + texto STT por USB CDC y los muestra en pantalla.

Instalacion (una sola vez):
    pip install pyserial opencv-python numpy

Uso:
    python tools/monitor_viewer.py COM3          (Windows)
    python tools/monitor_viewer.py /dev/ttyACM1  (Linux)

El puerto es el mismo COM que usa idf.py monitor (USB Serial/JTAG del ESP32-P4).
idf.py monitor y este script NO pueden estar abiertos al mismo tiempo.

Protocolo de framing — dos tipos de paquete:

  Tipo JPEG (frame de camara):
    [4]  magic: AB CD EF 01
    [4]  tamano JPEG en bytes (uint32 LE)
    [1]  zona: 0=NONE  1=LEFT  2=CENTER  3=RIGHT
    [1]  reservado
    [N]  datos JPEG (escala de grises)

  Tipo TEXT (resultado STT):
    [4]  magic: AB CD EF 02
    [4]  longitud del texto (uint32 LE, incluye NUL terminal)
    [1]  reservado
    [1]  reservado
    [N]  texto UTF-8 terminado en NUL
"""

import sys
import struct
import numpy as np
import cv2
import serial

MAGIC_JPEG = bytes([0xAB, 0xCD, 0xEF, 0x01])
MAGIC_TEXT = bytes([0xAB, 0xCD, 0xEF, 0x02])
HDR_SIZE   = 10
MAX_BUF    = 2 * 1024 * 1024
MAX_JPEG   = 500_000
MAX_TEXT   = 256

ZONE_LABEL = {0: "NONE", 1: "LEFT", 2: "CENTER", 3: "RIGHT"}
ZONE_COLOR = {0: (160, 160, 160), 1: (40, 120, 220), 2: (30, 130, 230), 3: (40, 180, 60)}


def find_any_magic(buf: bytearray):
    """Retorna (idx, tipo) del primer magic encontrado, o (-1, None)."""
    j = buf.find(MAGIC_JPEG)
    t = buf.find(MAGIC_TEXT)
    if j < 0 and t < 0:
        return -1, None
    if j < 0:
        return t, "text"
    if t < 0:
        return j, "jpeg"
    if j <= t:
        return j, "jpeg"
    return t, "text"


def extract_packet(buf: bytearray):
    """
    Extrae el primer paquete completo del buffer.
    Retorna (tipo, payload, zona_o_None, buf_restante).
    tipo puede ser "jpeg" o "text".
    Retorna (None, None, None, buf_recortado) si necesita mas datos.
    """
    while True:
        idx, tipo = find_any_magic(buf)
        if idx < 0:
            return None, None, None, bytearray(buf[-3:])

        if len(buf) - idx < HDR_SIZE:
            return None, None, None, bytearray(buf[idx:])

        size = struct.unpack_from('<I', buf, idx + 4)[0]

        if tipo == "jpeg":
            zone = buf[idx + 8]
            if size == 0 or size > MAX_JPEG:
                buf = buf[idx + 4:]
                continue
            end = idx + HDR_SIZE + size
            if len(buf) < end:
                return None, None, None, bytearray(buf[idx:])
            payload = bytes(buf[idx + HDR_SIZE:end])
            return "jpeg", payload, zone, bytearray(buf[end:])

        else:  # text
            if size == 0 or size > MAX_TEXT:
                buf = buf[idx + 4:]
                continue
            end = idx + HDR_SIZE + size
            if len(buf) < end:
                return None, None, None, bytearray(buf[idx:])
            raw = buf[idx + HDR_SIZE:end]
            text = raw.rstrip(b'\x00').decode("utf-8", errors="replace")
            return "text", text, None, bytearray(buf[end:])


def overlay_zone(frame_bgr, zone: int, stt_text: str) -> None:
    """Dibuja barra de zona (arriba) y texto STT (abajo) sobre el frame."""
    h, w = frame_bgr.shape[:2]

    # Barra superior — zona de obstáculo
    label = ZONE_LABEL.get(zone, "?")
    color = ZONE_COLOR.get(zone, (200, 200, 200))
    cv2.rectangle(frame_bgr, (0, 0), (w, 50), (25, 25, 25), -1)
    cv2.putText(frame_bgr, f"SENTIS  zona: {label}",
                (12, 36), cv2.FONT_HERSHEY_SIMPLEX, 1.1, color, 2, cv2.LINE_AA)

    # Barra inferior — último comando STT reconocido
    if stt_text:
        cv2.rectangle(frame_bgr, (0, h - 50), (w, h), (20, 20, 20), -1)
        cv2.putText(frame_bgr, f"STT: {stt_text}",
                    (12, h - 14), cv2.FONT_HERSHEY_SIMPLEX, 0.9, (80, 220, 80), 2, cv2.LINE_AA)


def main(port: str) -> None:
    print(f"Conectando a {port} ...")
    try:
        ser = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as e:
        print(f"Error: {e}")
        sys.exit(1)

    print("Conectado. Esperando frames del ESP32-P4... (ESC para salir)")
    buf        = bytearray()
    frames_rx  = 0
    last_zone  = 0
    last_stt   = ""

    with ser:
        while True:
            chunk = ser.read(8192)
            if chunk:
                buf += chunk

            if len(buf) > MAX_BUF:
                print("[warn] buffer overflow — resincronizando")
                buf.clear()
                continue

            tipo, payload, extra, buf = extract_packet(buf)
            if tipo is None:
                continue

            if tipo == "text":
                last_stt = payload
                print(f"  STT: \"{last_stt}\"")
                continue

            # tipo == "jpeg"
            zone = extra
            last_zone = zone
            img = cv2.imdecode(np.frombuffer(payload, dtype=np.uint8), cv2.IMREAD_COLOR)
            if img is None:
                continue

            img = cv2.rotate(img, cv2.ROTATE_90_CLOCKWISE)
            img = cv2.flip(img, 1)
            buf = bytearray()   # descartar buffer para mostrar siempre el frame reciente

            frames_rx += 1
            display = img
            overlay_zone(display, last_zone, last_stt)
            cv2.imshow("SENTIS Monitor", display)

            if frames_rx % 30 == 0:
                print(f"  {frames_rx} frames  |  {len(payload)} bytes  "
                      f"zona: {ZONE_LABEL.get(last_zone,'?')}  stt: \"{last_stt}\"")

            if cv2.waitKey(1) == 27:   # ESC
                break

    cv2.destroyAllWindows()
    print(f"Cerrado. Total frames: {frames_rx}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    main(sys.argv[1])
