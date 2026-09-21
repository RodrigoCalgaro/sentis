#!/usr/bin/env python3
"""Standalone serial capture, sin depender de idf.py monitor.

Motivo: en este entorno (Windows, tool de shell no interactivo), `idf.py
monitor` con la salida redirigida a un archivo no sirve para captura
automatizada — el proceso corre indefinidamente y tanto PowerShell como el
buffering de stdout de Python bufferean todo hasta que el proceso termina
(o sea, nunca). Este script en cambio es un proceso que TERMINA SOLO tras
`--seconds` segundos, así que el archivo de log queda completo y listo para
leer apenas el proceso sale.

Uso:
    python tools/serial_capture.py --port COM3 --seconds 30 --out boot.log

Antes de correrlo, cerrar cualquier `idf.py monitor` / esp_idf_monitor
huérfano que tenga el puerto abierto (en Windows: buscar procesos python.exe
con ese puerto en la línea de comandos y matarlos), o vas a obtener un
"Acceso denegado" al abrir el puerto.
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("Falta pyserial. Instalar con: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", required=True, help="Puerto COM, p.ej. COM3")
    ap.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    ap.add_argument("--seconds", type=float, default=20.0, help="Duracion de la captura")
    ap.add_argument("--out", required=True, help="Archivo de salida")
    ap.add_argument(
        "--reset-on-open",
        action="store_true",
        help="Pulsar DTR/RTS al abrir para forzar un reset del target (como hace idf.py monitor)",
    )
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"No se pudo abrir {args.port}: {e}", file=sys.stderr)
        print(
            "Si dice 'Acceso denegado', puede haber un idf.py monitor / "
            "esp_idf_monitor huerfano con el puerto abierto — cerrarlo primero.",
            file=sys.stderr,
        )
        return 1

    if args.reset_on_open:
        # Secuencia clasica esptool/idf_monitor: RTS baja reset, DTR baja
        # GPIO0 — soltar ambos deja correr el firmware normal.
        ser.rts = True
        ser.dtr = False
        time.sleep(0.1)
        ser.rts = False
        time.sleep(0.1)

    t_end = time.time() + args.seconds
    total = 0
    with open(args.out, "wb") as f:
        while time.time() < t_end:
            chunk = ser.read(4096)
            if chunk:
                f.write(chunk)
                f.flush()
                total += len(chunk)

    ser.close()
    print(f"Captura terminada: {total} bytes en {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
