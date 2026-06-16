# Bitácora de depuración — integración LiDAR SP10M01

Sensor: **SP10M01**, DToF de punto único, 10 m, 100 Hz, UART TTL.
Placa: **ESP32-P4-Module-DEV-KIT (Waveshare)**. Objeto de prueba a ~30 cm.

## Asignación de pines (constante durante toda la sesión)

| Señal del sensor | Cable    | Pin ESP32-P4 | Notas |
|------------------|----------|--------------|-------|
| TX (sensor→ESP)  | Rojo     | GPIO4 (RX)   | Recepción |
| RX (ESP→sensor)  | Negro    | GPIO5 (TX)   | Transmisión |
| VCC              | Verde    | 3V3 (riel)   | Ver cambios abajo |
| GND              | Amarillo | GND (riel)   | Masa común |

> Los **pines lógicos no cambiaron** (RX=GPIO4, TX=GPIO5). Lo que cambió fue la **alimentación** (VCC) y, hacia el final, descubrimos que rojo y negro son **la misma línea física** (sensor half-duplex de un solo cable).

## Cambios de cableado

| # | Cambio | Motivo | Resultado |
|---|--------|--------|-----------|
| W1 | VCC (verde): GPIO2 → **3V3 riel** | Un GPIO entrega ~20-40 mA; el lidar pide más → brownout. (El verde se había movido a GPIO2 en sesión previa por una hipótesis de timing equivocada.) | Sensor correctamente alimentado a 3.3V |
| W2 | Probado VCC a **5V**, descartado | La spec dice 5V, pero el TX del sensor da idle ~4.5V a 5V → no tolerable por GPIO del ESP-P4 (no 5V-tolerant) sin level-shifter. La unidad funciona a 3.3V (idle 2.8V, lee bien como HIGH) → es variante 3.3V | Se mantiene 3.3V |
| W3 | Desconexión de rojo / negro (pruebas) | Aislar si el "eco" venía del corto o del sensor | Confirmó línea única (ver R7-R9) |
| **W4 (pendiente)** | **Resistor ~1kΩ en serie en TX (GPIO5)** | Half-duplex de un cable: TX push-pull idle-alto suprime al sensor. Con 1kΩ en serie, el sensor gana la línea al recibir; TX sigue push-pull (sin tocar modos de pin) | A probar cuando consiga el resistor |

## Cronología de pruebas (firmware)

| # | Qué probamos | Observado | Por qué cambiamos |
|---|--------------|-----------|-------------------|
| F1 | Estado inicial: 115200, parser TFMini (`0x59 0x59`), VCC en GPIO2 | 512 bytes casi todos `0xFF` con bits caídos; sin candidatos de distancia | Firma de baud equivocado → barrer bauds |
| F2 | Barrido de bauds, scoring = nº de valores distintos | Los 9 bauds dan ~130-150 distintos, sin destacarse ninguno | El conteo de distintos no discrimina datos vs ruido → usar periodicidad |
| F3 | Scoring = autocorrelación (periodicidad de frame) | Rampa 2-7% sin pico a ningún baud | Sin estructura periódica → es ruido, no datos enmarcados |
| F4 | Pull-up en RX + baseline con sensor apagado | Baseline OFF = 0 bytes (RX limpio); con pull-up el "ruido" desaparece | Confirmó que las "data" eran ruido de línea flotante; sospecha de alimentación (GPIO2) |
| F5 | (tras W1) VCC al riel 3V3, quitado control GPIO2 | Todo SILENT en los 9 bauds | El sensor está vivo pero no transmite estando estable |
| F6 | "Arrival watch": escuchar mientras se power-cicla el verde a mano | **23 bytes** en el momento del power-up, después silencio | El sensor emite una ráfaga al encender y se calla → no auto-emite; necesitaría comando |
| F7 | Identificado protocolo **familia DTS6012M** (frame de 23 bytes); probe activo enviando `START_STREAM` (`A5 03 20 01 00 00 00`+CRC) a 921600+ | 0 bytes | El comando se manda tras el boot (ráfaga ya pasó) y no hay respuesta |
| F8 | Captura de ráfaga @115200 + envío START_STREAM | Ráfaga 25 bytes (`08 00…54 FF FF`); START_STREAM **vuelve byte-por-byte** (`A5 03 20 01 00 00 00 02 6E`) | Eco de nuestro propio comando = TX y RX puenteados eléctricamente |
| F9 | Continuidad GPIO4↔GPIO5 | **Abierto** (sin corto en protoboard) | El eco vuelve por el sensor/cable, no por la placa |
| F10 | Continuidad rojo↔negro + test desconectando rojo | Rojo↔negro **con continuidad**; sin rojo el eco desaparece | **Confirmado: sensor half-duplex, una sola línea de datos** (rojo y negro = mismo nodo) |
| F11 | TX en open-drain (liberar la línea al estar en reposo) | 0 bytes, ni eco | `gpio_set_direction(OUTPUT_OD)` tras INPUT desconecta el ruteo UART del pad |
| F12 | 2 fases: escuchar con TX liberado (INPUT) + TX half-duplex con release | 0 bytes en ambas | Alternar dirección de pin rompe el ruteo UART (fragilidad ESP-IDF) |
| F13 | "Clean listen" @115200 con **negro desconectado** (cero contención) | Solo bytes en cada power-cycle (transitorio de apagado: `FF…00`); silencio total entre cycles | Estable = mudo (confirma necesita comando); el transitorio no es un frame |
| F14 | Single-wire open-drain en GPIO4 con re-vinculación explícita de señales UART (`esp_rom_gpio_connect_*_signal`, `INPUT_OUTPUT_OD`) + envío START_STREAM | **9 bytes (eco limpio) en todos los bauds**, `data frame no` | **BUS HALF-DUPLEX RESUELTO**: TX y RX confiables sobre un cable, pull-up interno alcanza a 115200. Pero el sensor **ignora el comando DTS** → el SP10M01 no es DTS a nivel de comando |
| F15 | Reverse-engineer: barrer bauds capturando la ráfaga de power-up | Solo transitorios de VCC (`FF→denso→00`), iguales a todo baud, sin header repetible | La "ráfaga" siempre fue transitorio eléctrico, no datos. El sensor NUNCA emite espontáneamente |
| F16 | Comando trigger Benewake TFmini (`5A 04 04 62`), buscar respuesta `59 59` | `echo=20/20`, `frame=0/20` a todo baud | **BUS 100% SANO** (TX/RX confiables tras reasentar pin). Pero el sensor **ignora el trigger TFmini** también |
| F17 | Deep-research + brute-force de 25+ comandos (DTS, Benewake, TOFSense, Modbus, AT, single-bytes, LSLIDAR A5 5A) a 9 baud rates | Solo ecos en todo; escucha pasiva a 9 bauds = silencio total | **PROTOCOLO COMPLETAMENTE PROPIETARIO.** El sensor recibe (eco=20/20), no responde a ningún protocolo conocido. Único camino restante: contactar al vendedor. |

## Hechos confirmados

- El sensor está **vivo y bien alimentado a 3.3V** (variante 3.3V; idle TX ~2.8V).
- **RX (GPIO4) está sano**: capta limpio cuando el sensor maneja la línea.
- **Es half-duplex de UNA sola línea**: los cables rojo y negro son el mismo nodo eléctrico.
- **No auto-emite**: estando estable está mudo; solo emite una ráfaga corta al power-up. → **Requiere un comando para empezar a medir.**
- Protocolo **probable**: familia **DTS6012M** (frame de 23 bytes, header `A5 03 20`, CRC-16/Modbus, comando `START_STREAM`=`0x01`). Aún **no confirmado** con datos limpios (la ráfaga nunca se decodificó con header `A5` por contención).
- **TX push-pull transmite bien** (logró eco de 9 bytes); el problema es que idle-alto **suprime** al sensor en el bus compartido.
- Alternar el modo del pin TX en ESP-IDF (INPUT/OPEN_DRAIN) **rompe el ruteo UART** → descartado como solución.

## Causa raíz (resuelta) y estado actual

**Causa raíz eléctrica (RESUELTA):** bus half-duplex de un solo cable. El TX push-pull del ESP, en reposo a nivel alto, peleaba contra el TX del sensor sobre la misma línea, corrompiendo todo y suprimiendo al sensor.

**Solución que funcionó (F14):** un solo pin (GPIO4) en `GPIO_MODE_INPUT_OUTPUT_OD` con pull-up, y re-vinculación **explícita** de las señales UART TX y RX a ese pin vía la matriz de GPIO (`esp_rom_gpio_connect_out_signal` / `connect_in_signal`). El truco clave es re-vincular las señales a mano: `gpio_set_direction()`/`uart_set_pin()` desconectan el ruteo UART en este chip (esa era la causa de los 0 bytes). No hizo falta resistor (el pull-up interno alcanza a 115200; si a futuro fallara a baudios altos, el fallback es pull-up externo 4.7k / serie 1k).

**Cableado actual:** rojo → GPIO4 (única línea de datos), verde → 3V3, amarillo → GND. Negro/GPIO5 sin usar.

**Bus confirmado 100% sano (F16):** con el pin GPIO4 bien reasentado, eco de nuestro comando = 20/20 a todos los bauds. TX y RX confiables sobre el cable único. (Si el eco cae a 0/20, es jumper flojo de GPIO4 — reasentar.)

**Problema restante (de PROTOCOLO, no hardware):** el SP10M01 recibe nuestros bytes pero **no responde** ni al `START_STREAM` de DTS6012M ni al trigger `5A 04 04 62` de Benewake TFmini. No emite nada espontáneo (la "ráfaga" de power-up era solo transitorio de VCC). Su comando real no está en las hipótesis comunes ni documentado públicamente.

## Estado final y próximo paso

**El bus está completamente resuelto.** El firmware de diagnóstico ([lidar.c](lidar.c)) mantiene la configuración single-wire open-drain que funciona; con el comando correcto se lee la distancia en una sola corrida sin cambiar nada del hardware.

**El único bloqueante es el protocolo propietario.** Hemos probado exhaustivamente 25+ variantes de comandos de todas las familias conocidas de LiDAR DToF (DTS, Benewake, TOFSense, Modbus, AT, LSLIDAR) a 9 baud rates distintos. Ninguno produjo respuesta. El fabricante no ha publicado el protocolo.

**Acción requerida:** Contactar al vendedor (rcdrone.top / MercadoLibre) solicitando el **datasheet o manual de protocolo UART** del SP10M01. Con esos bytes, el resto es trivial.
