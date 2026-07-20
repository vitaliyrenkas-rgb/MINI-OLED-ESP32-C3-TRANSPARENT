# MINI OLEG — MINI-005 final drop-in sketch

Kyiv timestamp from ChatGPT session: 2026-07-17 11:44:35 Europe/Kyiv.

## Hardware target

- Board: ESP32-C3 Super Mini / Tenstar Robot style board
- Display: Waveshare 1.51" Transparent OLED, SSD1309, 128x64
- Display mode: factory 4-wire SPI
- OLED power: **VCC -> 3V3 only**. Do not power this OLED from 5V in this build.

## Hardware-passed OLED wiring

```text
OLED VCC -> 3V3
OLED GND -> GND
OLED DIN -> GPIO10
OLED CLK -> GPIO8
OLED CS  -> GPIO7
OLED DC  -> GPIO6
OLED RST -> GPIO5
BOOT     -> GPIO9  (on-board BOOT button, runtime config portal)
```

GPIO8 is used for OLED CLK in the confirmed hardware layout. GPIO9 is kept for BOOT/config portal.

## GPIO1 audio presence detector

```text
                         3V3
                          │
                      R2 100 kOhm
                          │
MH-M18 L/R -> C1 1 uF -> R1 12 kOhm -> node A -> GPIO1 / ADC
                                            │
                                        R3 100 kOhm
                                            │
                                           GND

MH-M18 GND -------------------------------- GND ESP32-C3
```

- C1: 1 uF ceramic/film, marking `105`.
- R1: 12 kOhm.
- R2/R3: 100 kOhm each.
- Use one line-level channel from MH-M18 before the amplifier. Do not connect speaker output.

## Behavior

- Audio detected on GPIO1 -> main clock/weather screen.
- No audio for 30 seconds -> sleep kitty.
- Audio returns -> main screen again within roughly 0.5 s.
- Weather update: on startup after Wi-Fi connection, then once per hour; failed weather retry every 60 s.
- First boot without config starts setup AP automatically.
- Runtime config portal: hold BOOT for about 5 s after the UI is already running.

## Libraries

- U8g2
- ArduinoJson
- ESP32 Arduino core with WiFi/WebServer/DNSServer/Preferences/HTTPClient
