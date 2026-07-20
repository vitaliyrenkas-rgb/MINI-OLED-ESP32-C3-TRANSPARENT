# MINI-021 — Clock Y=62 final correction

Final UI-only correction from the real OLED photo with red alignment guides.

- Clock X stays at `13`.
- Clock uses `u8g2_font_logisoso24_tn` in baseline mode.
- Clock cursor Y changes only from `60` to `62`.
- Battery, integrated XOR lightning, Wi-Fi icon, date, grid, ADC and all runtime logic are unchanged.

Exact changed line:

```cpp
static constexpr int CLOCK_BASELINE_Y = 62;
```


---

# MINI OLEG — MINI-020 Clock Frame Align

## Що виправлено

MINI-019 помилково використав `setFontPosBottom()` і підтягнув великі цифри
часу вгору до горизонтальної межі.

У MINI-020 повернуто нормальне baseline-позиціювання й задано дві точні
координати нижнього годинника:

```cpp
static constexpr int CLOCK_X = 13;
static constexpr int CLOCK_BASELINE_Y = 60;
```

Для `u8g2_font_logisoso24_tn` це дає видиму рамку цифр:

```text
верх часу: y = 34  — по верхній лінії Wi-Fi
низ часу:  y = 62  — по нижній лінії назви місяця
```

Батарея, внутрішня XOR-блискавка, Wi-Fi та решта UI не змінювались.

## Актуальне підключення OLED

```text
OLED VCC -> 3V3
OLED GND -> GND
OLED DIN -> GPIO10
OLED CLK -> GPIO8
OLED CS  -> GPIO7
OLED DC  -> GPIO6
OLED RST -> GPIO5
BOOT     -> GPIO9
```

## Дільник батареї

```text
BAT+ ---- 100 kOhm ----+---- GPIO1 / ADC1_CH1
                       |
                    100 kOhm
                       |
                      GND

BAT-/HU-009 GND ------------- ESP32-C3 GND
```
