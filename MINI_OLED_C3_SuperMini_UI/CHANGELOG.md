# Changelog

## v1.0.0 — 2026-07-22

First public hardware release.

### Added

- GPIO1 battery measurement through a 100 kΩ / 100 kΩ divider;
- averaged millivolt sampling and light smoothing;
- battery calibration constants;
- 9-pixel vertical battery icon;
- XOR lightning glyph integrated inside the battery;
- public wiring and calibration documentation;
- real assembled-device gallery.

### Changed

- large clock moved to `X=13`, baseline `Y=62`;
- GPIO1 reassigned from the retired audio detector to battery ADC;
- all public project naming standardized on **OLED**.

### Removed

- unreliable GPIO1 audio-presence runtime logic.
