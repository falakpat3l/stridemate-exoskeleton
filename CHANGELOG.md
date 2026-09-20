# Changelog

## [1.1.0] - 2026-09-20

Safety and security review of the 1.0.0 firmware. The assist algorithm and its
tuning values are unchanged; everything here is about what happens when
something goes wrong.

### Fixed
- **Sensor-gap lurch.** Re-acquiring a lost ToF target was differenced against
  a sample from before the gap, producing a false velocity of a few hundred
  mm/sample and commanding near-full power, often in the opposite direction.
  The filter is now re-seeded after a gap in valid readings, invalid readings
  report zero velocity, and per-sample velocity is clamped.
- **`motorOff()` could not remove drive.** The H-bridge enable pins were driven
  high once at boot and never touched, so every off path relied on the PWM
  peripheral. They are now dropped whenever the motor is not allowed.
- **No bound on a wedged I²C bus.** A stuck transaction stopped `loop()` while
  the LEDC peripheral kept applying the last duty. Both buses now time out and
  a watchdog resets the board if the control loop stalls.
- **CSRF on the control endpoints.** `/motor`, `/stop`, `/setPWM`,
  `/setSensitivity` and `/toggleMotor` were GET behind Basic auth, so any page
  in the same browser could enable the motor. They are POST with a same-origin
  check.
- **"Sensor ok" with no target.** A live sensor seeing nothing was reported as
  healthy and allowed to assist on a frozen distance.

### Added
- Open-loop motor thermal budget with smooth assist fold-back, shown per leg on
  the dashboard. **Ships disabled** (`THERMAL_PROTECTION false`): the constants
  are simulated, not measured. The estimate still runs and is still displayed,
  so it can be calibrated against real use before being switched on.
- `MOTOR_DIRECTION_SIGN`, so a mirrored left motor is a one-line config change.
- `tofTarget` and `thermal` / `leftThermal` in the telemetry JSON.

### Changed
- `TOF_TIMING_BUDGET_MS` pinned at 33 ms. Velocity is measured in mm per
  sample, so the sensor period is a control gain and must not follow the
  library default.
- Documented that the assist curve saturates at ~17 mm/sample, below walking
  speed, and that the dashboard battery is the logic supply rather than the
  motor pack.

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
