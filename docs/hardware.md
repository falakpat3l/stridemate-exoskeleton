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

The firmware assumes a **single-cell Li-ion** reading (3.0 V = 0%, 4.2 V = 100%) through a **2:1 divider**. If the pack is different (for example the 12 V motor battery), change `BATTERY_DIVIDER_RATIO`, `BATTERY_EMPTY_V` and `BATTERY_FULL_V` in `config.h`. The voltage at GPIO 34 must **never exceed 3.3 V**.

## Motor-driver notes

- PWM runs at 2 kHz with 8-bit resolution (duty 0–255).
- The firmware sets both PWM outputs to 0 *before* enabling `R_EN`/`L_EN` at power-up.
- Only one PWM pin is ever active at a time. On a direction change, the output drops to 0 for one control step and then ramps up again.
