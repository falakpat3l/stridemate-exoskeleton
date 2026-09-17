# Changelog

## [1.0.0] - 2026-09-17

First public release.

### Added
- `firmware/right_leg`: maintained right-leg firmware, split into `config.h`,
  `dashboard.h` and `secrets.h`.
- `firmware/left_leg`: reconstructed left-leg firmware (not yet hardware-tested).
- Dashboard: motor state, STOP button, left-leg online badge, sensor-fault
  badge, battery for both legs, offline chart (no internet needed).
- Safety: motor disabled at boot, sensor-stale cut-off, motor off during OTA,
  soft ramp on direction reversal, left leg stops if the right leg goes silent.
- Password protection for the dashboard, OTA updates and the leg-to-leg link.
- Documentation, MIT license, GitHub Actions compile check.

### Fixed
- MPU6050 bytes could be combined in the wrong order.
- Right-leg control loop could freeze for seconds when the left leg was offline.
- Only left-leg distance was parsed; velocity/PWM/pitch/roll now shown too.
- Dashboard inputs are range-checked on the ESP32.

### Original
- `original/right_leg_original`: prototype firmware by B Dileep Kumar, as
  handed over (Wi-Fi credentials removed).
