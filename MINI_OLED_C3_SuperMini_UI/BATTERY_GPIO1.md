# GPIO1 battery monitor

GPIO1 is dedicated to battery ADC through a 100 kΩ / 100 kΩ divider:

```text
BAT+ ---- 100 kΩ ----+---- GPIO1 / ADC1_CH1
                     |
                  100 kΩ
                     |
                    GND
```

The old audio-presence detector is retired. The HUD uses a 9-pixel vertical battery icon with an XOR lightning glyph. Final clock placement is `X=13`, baseline `Y=62`.
