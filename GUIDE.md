# Guide: how this code works

A plain-English map of the firmware. The detailed docs are in `docs/`; this page tells you which file to open.

> This drives motors strapped to a person's leg. Before changing anything that affects the motor, read [BEFORE-YOU-FLASH-THIS.md](BEFORE-YOU-FLASH-THIS.md) and [docs/safety.md](docs/safety.md), and test on the bench with the motor disconnected first.

## What it does

Each leg has its own ESP32 board. The board measures how fast the leg is moving with a distance sensor, and turns the motor on in the same direction to help. The right leg is the "main" board: it also runs the phone dashboard and fetches the left leg's data over Wi-Fi.

## The big picture

```
every 10 ms, on each leg:
  1. read the distance sensor -> smooth it -> work out leg speed
  2. read the tilt sensor (shown on the dashboard only)
  3. decide motor power:
       motor off if: not enabled, sensor problem, nothing in view, or a wireless update is running
       otherwise:    faster leg movement = more help, up to "Assist Strength"
                     (with thermal protection on, less help while the motor is hot)
  4. ramp the motor smoothly toward that power

right leg only, in the background:
  - every 100 ms, fetch the left leg's data and send it the latest settings
  - serve the dashboard web page and its buttons
```

## Where things live

| File | What it does | Open it when you want to... |
|---|---|---|
| `firmware/*/config.h` | Every setting: pins, IP addresses, limits, feature switches. Each one is commented | change a number or turn a feature on |
| `firmware/*/secrets.h` | Your Wi-Fi name, passwords, link key. Copy from `secrets.example.h`; never uploaded to GitHub | set up a new board |
| `firmware/right_leg/right_leg.ino` | Right leg program, in labelled sections: helpers, left-leg link, sensors, thermal budget, assist control, web server, setup and loop | change behaviour |
| `firmware/right_leg/dashboard.h` | The phone dashboard page (HTML stored inside the firmware) | change how the dashboard looks |
| `firmware/left_leg/left_leg.ino` | Left leg program. Same sections, minus the dashboard; its small web server answers the right leg | change behaviour on the left |
| `original/` | The first prototype code, kept untouched for reference | never edit |
| `docs/getting-started.md` | Step by step: install, settings, upload | first time setup |
| `docs/how-it-works.md` | The control loop and every tunable setting, in detail | understanding the maths |
| `docs/hardware.md` | Pin map and wiring | wiring |
| `docs/api.md` | The dashboard's web addresses (`/data`, `/motor`, `/stop`...) | talking to the board from another program |

The two `.ino` files share a lot of code on purpose. A change to the sensor or motor logic usually needs making in **both**.

## Common changes

**New Wi-Fi or hotspot.** Edit `secrets.h` in both folders. If the hotspot's addresses look different (for example `192.168.43.x`), change the IP lines at the top of both `config.h` files.

**Turn on an optional feature.** Look for the `..._ENABLED` and `THERMAL_PROTECTION` switches in `config.h`. Each one says what wiring it needs first.

**Change how much help the motor gives.** Use the Assist Strength and Sensitivity sliders on the dashboard. Only change the curve in `config.h` after reading `docs/how-it-works.md`.

## Check you didn't break anything

GitHub Actions compiles both programs on every push (see the badge on the README). On your Mac, the Arduino IDE's **Verify** button (the tick) does the same check without uploading.

## Words you will see

- **ToF (time-of-flight) sensor**: the VL53L1X; measures distance with a laser pulse.
- **IMU**: the MPU6050 tilt and motion sensor.
- **PWM duty**: motor power, from 0 (off) to 255 (full).
- **H-bridge**: the BTS7960 motor driver; lets the motor run both ways.
- **Low-pass filter**: smoothing that ignores quick jitter in a reading.
- **OTA**: "over the air", uploading new firmware over Wi-Fi instead of USB.
- **Thermal budget**: an estimate of how hot the motor is getting, since nothing measures it directly.
