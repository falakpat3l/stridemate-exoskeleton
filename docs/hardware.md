# Hardware

## Components (per leg)

| Part | Role |
|---|---|
| ESP32 development board (ESP32-WROOM-32, "ESP32 Dev Module") | Controller, Wi-Fi |
| VL53L1X time-of-flight distance sensor | Measures distance (mm) as the leg moves |
| MPU6050 accelerometer + gyroscope | Tilt (pitch/roll) telemetry |
| BTS7960 (IBT-2) dual H-bridge motor driver | Drives the motor in both directions |
| Windshield-wiper DC gear motor | Assist actuator |
| Battery + 2:1 resistor divider to the ADC pin | Battery-level monitoring |
| CNC-machined aluminium thigh links, 3D-printed housings | Structure |

Whole device: two of everything above, under 4 kg total.

## Pin map

The right-leg pins come from the original prototype firmware. **The left leg is assumed to use the same wiring.** Check this on the real hardware.

| Function | ESP32 pin | Connects to |
|---|---|---|
| Motor PWM, forward | GPIO 32 | BTS7960 `RPWM` |
| Motor PWM, reverse | GPIO 33 | BTS7960 `LPWM` |
| Motor enable, forward | GPIO 27 | BTS7960 `R_EN` |
| Motor enable, reverse | GPIO 14 | BTS7960 `L_EN` |
| Battery voltage | GPIO 34 (ADC) | Middle of the 2:1 divider |
| ToF sensor SDA | GPIO 25 | VL53L1X `SDA` (I2C bus 0, 10 kHz) |
| ToF sensor SCL | GPIO 26 | VL53L1X `SCL` |
| IMU SDA | GPIO 21 | MPU6050 `SDA` (I2C bus 1, 100 kHz) |
| IMU SCL | GPIO 22 | MPU6050 `SCL` |

Power the sensors from **3.3 V** and connect **all grounds together**: ESP32, sensors, motor driver and battery.

## Battery measurement

The firmware assumes a **single-cell Li-ion** reading (3.0 V = 0%, 4.2 V = 100%) through a **2:1 divider**. GPIO 34 at 11 dB attenuation tops out around 3.1 V, so the highest pack voltage this can represent is roughly 6.6 V.

**The wiper motors run on 12 V, so this reading is the logic supply, not the motor pack.**

A second divider on `PIN_PACK_ADC` (GPIO 35 by default) fixes that: set `PACK_DIVIDER_RATIO` to match your resistors and `PACK_EMPTY_V` / `PACK_FULL_V` to your pack, then `PACK_MONITOR_ENABLED true`. The firmware then publishes `packPercent` alongside the logic battery. The dashboard labels it accordingly. If you want the pack on the dashboard too, add a second divider on a spare ADC pin — a sagging motor pack changes the torque a given duty produces, and right now nothing would show it. The voltage at any ADC pin must **never exceed 3.3 V**.

## Motor-driver notes

- PWM runs at 2 kHz with 8-bit resolution (duty 0–255). Higher frequencies move the whine out of the audible band and reduce current ripple in the motor, at the cost of more switching loss in the BTS7960 — worth trying on the bench, not worth changing blind.
- The firmware sets both PWM outputs to 0 *before* enabling `R_EN`/`L_EN`, and now **drops `R_EN`/`L_EN` low whenever the motor is not allowed to run**, so the motor coasts instead of the bridge sitting energised with 0% duty. The enables are the only path that removes drive independently of the PWM peripheral.

### Recommended addition: current sense  (firmware ready — `CURRENT_SENSE_ENABLED`)

The BTS7960 breakout exposes `R_IS` and `L_IS` current-sense outputs, and they are currently unused. Wiring one to a spare ADC pin through a sense resistor would give real motor current, which is what the firmware needs for genuine stall detection and thermal protection — right now both are estimated from duty cycle alone. This is the single highest-value hardware change on the list.

**The firmware side is already written.** Wire one `IS` pin through a resistor to ground at `PIN_CURRENT_ADC` (GPIO 36 by default), set `CURRENT_SENSE_RESISTOR_OHMS` and `CURRENT_SENSE_RATIO` to match your parts, measure the zero-current reading into `CURRENT_ZERO_OFFSET_MV`, then set `CURRENT_SENSE_ENABLED true`. Nothing else changes.
- Only one PWM pin is ever active at a time. On a direction change, the output drops to 0 for one control step and then ramps up again.
