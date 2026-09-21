#!/usr/bin/env python3
"""
SENTIS Link Test Client

Simula del lado de la PC lo que va a hacer la app Android companion, para
validar el protocolo de components/link/ (protocolo TCP sobre el SoftAP de
components/wifi) ANTES de escribir la app real. Mismo espiritu que
tools/monitor_viewer.py para el protocolo USB, pero para este link WiFi.

Instalacion (una sola vez):
    pip install ninguna dependencia extra — solo libreria estandar

Uso:
    1. Conectar el celular/PC a la red WiFi "SENTIS" (ver
       components/wifi/Kconfig.projbuild para SSID/password).
    2. python tools/link_test_client.py                  (default 192.168.4.1:3333)
    3. Mientras firmware/link.c corra su tarea de AUTOTEST TEMPORAL, este
       script va a mostrar los chunks de audio sintetico que llegan y los
       pedidos de OCR sinteticos cada 10s. Responder un pedido de OCR con
       el comando de consola "ocr <texto>".

Protocolo de framing (ver components/link/link.h):
    [4]  magic: 0x314E4553 ("SEN1", uint32 LE)
    [1]  tipo: 1=AUDIO 2=OCR_REQUEST 3=OCR_RESULT 4=COMMAND
    [3]  reservado
    [4]  tamano del payload (uint32 LE)
    [N]  payload

Payload de COMMAND: int32 command_id (LE) + char text[64] (UTF-8, NUL-padded).
Payload de OCR_RESULT: texto UTF-8 crudo, sin structura adicional.
"""

import socket
import struct
import sys
import threading
import time

DEFAULT_HOST = "192.168.4.1"
DEFAULT_PORT = 3333

LINK_MAGIC = 0x314E4553
HEADER_FMT = "<IB3xI"  # magic, type, pad(3), size
HEADER_LEN = struct.calcsize(HEADER_FMT)

MSG_AUDIO = 1
MSG_OCR_REQUEST = 2
MSG_OCR_RESULT = 3
MSG_COMMAND = 4

COMMAND_TEXT_MAX = 64

# Mismo texto que components/stt/stt.c s_commands[] — solo para que "cmd <id>"
# mande algo razonable sin tener que escribir el texto a mano cada vez.
COMMANDS_BY_ID = {
    1: "stop",
    2: "turn left",
    3: "turn right",
    4: "alert on",
    5: "help",
    6: "start reading",
    7: "stop reading",
}


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return buf


def send_framed(sock, msg_type, payload=b""):
    header = struct.pack(HEADER_FMT, LINK_MAGIC, msg_type, len(payload))
    sock.sendall(header + payload)


def send_command(sock, command_id, text=None):
    if text is None:
        text = COMMANDS_BY_ID.get(command_id, "")
    text_bytes = text.encode("utf-8")[: COMMAND_TEXT_MAX - 1]
    payload = struct.pack(f"<i{COMMAND_TEXT_MAX}s", command_id, text_bytes)
    send_framed(sock, MSG_COMMAND, payload)
    print(f"-> COMMAND id={command_id} texto={text!r}")


def send_ocr_result(sock, text):
    payload = text.encode("utf-8")
    send_framed(sock, MSG_OCR_RESULT, payload)
    print(f"-> OCR_RESULT texto={text!r}")


class ReaderThread(threading.Thread):
    def __init__(self, sock):
        super().__init__(daemon=True)
        self.sock = sock
        self.audio_chunks = 0
        self.audio_bytes = 0
        self._last_report = time.time()

    def run(self):
        while True:
            header = recv_exact(self.sock, HEADER_LEN)
            if header is None:
                print("[reader] conexion cerrada por el ESP32")
                return
            magic, msg_type, size = struct.unpack(HEADER_FMT, header)
            if magic != LINK_MAGIC:
                print(f"[reader] magic invalido: {magic:#x} — cierro")
                return
            payload = recv_exact(self.sock, size) if size else b""
            if payload is None:
                print("[reader] conexion cerrada leyendo payload")
                return
            self._handle(msg_type, payload)

    def _handle(self, msg_type, payload):
        if msg_type == MSG_AUDIO:
            self.audio_chunks += 1
            self.audio_bytes += len(payload)
            now = time.time()
            if now - self._last_report >= 2.0:
                print(f"[audio] {self.audio_chunks} chunks, {self.audio_bytes} bytes recibidos")
                self._last_report = now
        elif msg_type == MSG_OCR_REQUEST:
            fname = f"link_test_ocr_request_{int(time.time())}.jpg"
            with open(fname, "wb") as f:
                f.write(payload)
            print(f"[ocr] pedido de lectura recibido ({len(payload)} bytes) — guardado en {fname}")
            print('      respondé con: ocr <texto reconocido>')
        elif msg_type == MSG_COMMAND:
            # No debería llegar (COMMAND es celular->ESP32), pero no rompemos si pasa.
            print(f"[?] COMMAND inesperado del ESP32 ({len(payload)} bytes)")
        else:
            print(f"[?] tipo de mensaje desconocido {msg_type} ({len(payload)} bytes)")


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_HOST
    port = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_PORT

    print(f"Conectando a {host}:{port}...")
    sock = socket.create_connection((host, port), timeout=10)
    sock.settimeout(None)
    print("Conectado.")

    reader = ReaderThread(sock)
    reader.start()

    print(
        "\nComandos:\n"
        "  cmd <id> [texto]   manda un comando de voz (1-7, ver COMMANDS_BY_ID)\n"
        "  ocr <texto>        responde el ultimo pedido de OCR con este texto\n"
        "  stats              muestra contadores de audio\n"
        "  quit               cierra\n"
    )

    try:
        for line in sys.stdin:
            line = line.strip()
            if not line:
                continue
            if line in ("quit", "exit"):
                break
            parts = line.split(maxsplit=2)
            if parts[0] == "cmd" and len(parts) >= 2:
                cmd_id = int(parts[1])
                text = parts[2] if len(parts) > 2 else None
                send_command(sock, cmd_id, text)
            elif parts[0] == "ocr" and len(parts) >= 2:
                send_ocr_result(sock, line[len("ocr "):])
            elif parts[0] == "stats":
                print(f"audio: {reader.audio_chunks} chunks, {reader.audio_bytes} bytes")
            else:
                print("comando no reconocido, ver la lista de arriba")
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()


if __name__ == "__main__":
    main()
