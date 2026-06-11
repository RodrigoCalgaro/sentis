Protoboard rail +  ←── ESP32 3V3  (jumper M-F)
Protoboard rail -  ←── ESP32 GND  (jumper M-F)

Motor izquierdo:
  VCC ──── rail +   (jumper M-M dentro de protoboard)
  GND ──── rail -   (jumper M-M dentro de protoboard)
  SIG ──── GPIO3    (jumper M-F)

Motor derecho:
  VCC ──── rail +   (jumper M-M dentro de protoboard)
  GND ──── rail -   (jumper M-M dentro de protoboard)
  SIG ──── GPIO6    (jumper M-F)

SP10M01 wire        Protoboard          ESP32-P4 header
─────────────────────────────────────────────────────────
Verde  (3.3V) ──── rail +  ←─── 3V3   (NO usar GPIO: no da corriente suficiente)
Amarillo (GND) ─── rail -  ←─── GND
Rojo   (TX)   ──── fila libre ──── jumper M-F ──── GPIO4
Negro  (RX)   ──── fila libre ──── jumper M-F ──── GPIO5

# El intento de alimentar VCC desde GPIO2 dejaba al sensor en brownout
# (un GPIO entrega ~20-40mA; el lidar pide más). Para control de encendido
# por software, usar un MOSFET de canal P en high-side, no el GPIO directo.