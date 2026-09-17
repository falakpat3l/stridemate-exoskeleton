# Changes from the original prototype firmware

The original right-leg code is preserved in [`original/right_leg_original`](../original/right_leg_original/right_leg_original.ino); only the Wi-Fi credentials were removed. This page lists every intentional change in [`firmware/right_leg`](../firmware/right_leg) so the original author, or anyone else, can review it.

The **control algorithm and all tuning values are unchanged** (distance window, filters, PWM curve, slider ranges, pins, IPs).

## Security

| Change | Why |
|---|---|
| Wi-Fi name/password moved to `secrets.h`, which git ignores | Credentials were hard-coded and would have been published |
| OTA updates need a password | Anyone on the network could previously flash new firmware |
| Dashboard login (HTTP Basic auth) | Anyone on the network could previously enable the motor |
| Settings sent to the left leg need a shared `LINK_KEY` | Stops other devices from changing left-leg settings |

## Bugs fixed

| Issue | Original behaviour | Now |
|---|---|---|
| **MPU6050 byte order** | `read() << 8 \| read()`: C++ doesn't guarantee which `read()` runs first, so the high and low bytes could be swapped | Bytes read one at a time, in a fixed order |
| **Control loop blocked by the left leg** | `getLeftLegData()` ran every loop with the default HTTP timeout (several seconds). If the left leg was off, motor control froze with the last PWM still applied | Left-leg polling moved to a background task on the other CPU core with 150 ms timeouts |
| **Left-leg data incomplete** | Only the first JSON value (distance) was parsed. Velocity, PWM, pitch and roll always showed 0 | All fields parsed by name. Still falls back to "first value = distance" for the old left-leg firmware |
| **Distance sensor polling** | `distance()` was called every loop without `dataReady()`/`clearInterrupt()`, so the same sample could be re-read, making velocity jump between 0 and large values | Velocity is computed once per new measurement |
| **Unchecked dashboard inputs** | `/setPWM?value=9999` was accepted | Values clamped (assist 80–255, sensitivity 1–20) |
| **Sensor init not checked** | Silent failure if the VL53L1X or MPU6050 wasn't connected | Errors printed. ToF init retried every 2 s |
| **Wi-Fi wait** | Board waited forever for Wi-Fi in `setup()` | Tries for 15 s, then keeps reconnecting in the background |

## Safety additions

- Motor **disabled at boot** (`MOTOR_ENABLED_AT_BOOT`). The original started enabled at full assist strength.
- Motor off immediately (no ramp-down) when disabled, on sensor timeout, or during OTA.
- Explicit `/motor?on=` and `/stop` endpoints. The dashboard shows the real motor state. A single "toggle" button could end up in the wrong state if a tap was lost.
- Soft restart on direction reversal (`RESET_RAMP_ON_REVERSAL`).
- Optional right-motor stop when the left leg is offline (`STOP_MOTOR_IF_LEFT_OFFLINE`, off by default, as in the original).
- PWM set to 0 *before* the H-bridge enable pins go high.

## Performance and usability

- Dashboard HTML stored in flash (`PROGMEM`) instead of being rebuilt as a `String` on every page load.
- Chart.js (from the internet) replaced with a small built-in chart, so the dashboard works on a hotspot **without internet**.
- Dashboard polls one request at a time instead of stacking a new one every 100 ms. Slider updates are throttled.
- New dashboard info: left-leg online/offline, sensor fault, battery for both legs, uptime.
- Wi-Fi power-save disabled for lower latency.
- Battery read with the ESP32's calibrated `analogReadMilliVolts()`, averaged over 8 samples, once per second.
- Serial output throttled to 10 lines per second.
- All tunable values collected in `config.h`.

## Behaviour differences to be aware of

- **Velocity timing:** velocity is now "mm per new sensor sample", updated only when the sensor reports. In the original, the effective rate depended on how long the loop (including the blocking HTTP call) took. The sensitivity threshold may need a small re-tune on the real device.
- **Pitch/roll** use the original formulas, so the values match the old dashboard. Strictly speaking, `atan2(AcY, AcZ)` is usually called *roll*; the names were kept for continuity.
