# Mini OLED v1.0.0 — Transparent HUD

The first public hardware release of the transparent HU-009 Bluetooth speaker HUD.

## Highlights

- ESP32-C3 Super Mini + transparent 1.51-inch SSD1309 OLED;
- Ukrainian one-screen weather clock;
- Wi-Fi setup portal, NTP and OpenWeather;
- battery monitoring on GPIO1 through a 100 kΩ / 100 kΩ divider;
- compact vertical battery icon with an internal XOR lightning glyph;
- final clock alignment at `X=13`, baseline `Y=62`;
- GPIO9 configuration button support;
- real-device photos and wiring documentation.

## Upgrade note

The public build uses new OLED-only AP, hostname and storage identifiers. After upgrading from an internal pre-release build, open the setup portal and enter the connection settings once again.

## Hardware status

The final HUD and assembled device were tested on the real HU-009 / ESP32-C3 / transparent OLED build. A clean Arduino IDE compile is still recommended before publishing binaries.
