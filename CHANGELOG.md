# Changelog

## [1.3.0] - 2026-09-21

### Changed
- **The assist curve is now proportional by default** (`ASSIST_CURVE_MODE 1`),
  settling the decision recorded in issue #7.

  The old curve reached its 255 ceiling at about 17 mm per sensor reading and
  gave full power beyond that - a switch rather than a dial. It also shifted
  whenever the sensor's timing budget changed. Mode 1 measures speed in mm/s
  against the real interval between readings and rises evenly across the
  range, so neither is true any more.

  **This changes motor behaviour and has not been tested on hardware.**
  Sensitivity now means mm/s rather than mm per sample, so it needs re-tuning
  on the bench. `ASSIST_CURVE_MODE 0` restores the original curve exactly.

### Added
- A "Who continues this" section in the README and in
  BEFORE-YOU-FLASH-THIS.md, recording that the hardware is at IIT Hyderabad
  with B Dileep Kumar, that the `needs-bench-test` and `hardware` issues stay
  open for that reason, and that whoever carries the work forward holds the
  copyright in what they write.

## [1.2.0] - 2026-09-21

Completes the software side of the open hardware issues, so the remaining
work on a real device is wiring and confirming rather than designing.
**Everything hardware-dependent ships disabled**, so nothing changes on a
device until someone deliberately enables it.

### Added
- `BEFORE-YOU-FLASH-THIS.md`: an orientation document for whoever picks this
  up next, stating plainly what is measured, what is guessed and what has
  never been switched on. Linked from the README warning.
- Dashboard rows for motor-pack charge, peak thermal load and measured motor
  current, plus an unmissable banner when a stall has latched a motor off and
  how to clear it. The right leg relays the left leg's versions of all four.
- `ASSIST_CURVE_MODE`. Mode 0 is the existing per-sample curve; mode 1 is
  proportional in mm/s, measured against the real interval between valid
  samples, so the tuning no longer depends on `TOF_TIMING_BUDGET_MS`.
  Default 0. Mode 1 needs Sensitivity re-tuned - its units change.
- Motor current sensing on the BTS7960 `IS` pin (`CURRENT_SENSE_ENABLED`).
  When on, the thermal model integrates real I2t instead of duty^2, and a
  stalled motor is detected and latched off rather than inferred.
- Motor-pack voltage monitoring on a second divider
  (`PACK_MONITOR_ENABLED`), so the dashboard can show the 12 V pack and not
  only the logic supply.
- `thermalPeak` and a calibration line on serial, so a bench session
  produces the numbers the thermal constants need.
- Telemetry: `thermalPeak`, `stall`, `packPercent`, `currentA`.

### Fixed
- The thermal model integrated over a fixed `CONTROL_PERIOD_MS`. The same
  loop serves HTTP and OTA, so the real interval is longer whenever it is
  busy and the heat estimate read low. It now uses measured elapsed time.
- The MPU6050's gyro was read and discarded, leaving tilt from the
  accelerometer alone - noisiest exactly while walking, which is when it is
  being looked at. Added a complementary filter (`USE_GYRO_FUSION`).

### Changed
- Corrected the assist-curve documentation. Pinning `TOF_TIMING_BUDGET_MS`
  at 33 ms in 1.1.0 already moved the saturation point to about 515 mm/s.
  The earlier "bang-bang" description overstated what was left: the
  remaining issues are the curve's convex shape and its dependence on the
  sample period, which is what mode 1 addresses.

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
- Documentation, licence, GitHub Actions compile check.

### Fixed
- MPU6050 bytes could be combined in the wrong order.
- Right-leg control loop could freeze for seconds when the left leg was offline.
- Only left-leg distance was parsed; velocity/PWM/pitch/roll now shown too.
- Dashboard inputs are range-checked on the ESP32.

### Original
- `original/right_leg_original`: prototype firmware by B Dileep Kumar, as
  handed over (Wi-Fi credentials removed).
