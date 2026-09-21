# Before you flash this

> Read this first. It is the honest state of the project — what is measured,
> what is guessed, and what has never been switched on. The other docs tell
> you how things work; this one tells you what to distrust.

StrideMate is a **research prototype**, not a medical device. Nothing here has
been clinically validated or certified. Motors strong enough to move a leg can
also injure one.

---

## The one-paragraph version

The firmware compiles, the safety logic is sound, and the control law has been
simulated. **Almost none of it has been re-tested on the physical device.** The
right-leg firmware was rewritten from a working prototype and has not been run
since. The left-leg firmware was *reconstructed from scratch* and has never run
at all. Several features are written, complete, and deliberately switched off
because nobody has wired the hardware they depend on.

If you have the device, your job is mostly confirming things, not designing
them. Start with `docs/safety.md`.

---

## What has never touched hardware

| | Status |
|---|---|
| Right-leg firmware (post-rewrite) | Compiles in CI. Not flashed since the rewrite. |
| Left-leg firmware | **Reconstructed. Never run.** Pin map, sensor placement and motor direction are all assumptions. |
| Sensor-gap fix | Verified in simulation only. |
| Thermal model | Constants chosen in simulation. Never measured. |
| Gyro tilt fusion | Sign depends on how the IMU is mounted. Unverified. |

**The single most important physical test** is in `docs/safety.md` step 6:
block the distance sensor with your hand for a second, then uncover it. The
motor must not lurch. That was the worst bug found in review — a re-acquired
target used to produce a false velocity of about −238 mm/sample and drive the
motor to 58% duty *in reverse* within two samples. It is fixed in code. It has
only ever been proven in a simulation.

---

## Switches that ship OFF, and why

Each of these is finished firmware waiting on wiring. None of them changes
anything until you turn it on.

| Flag | What it needs | What you get |
|---|---|---|
| `CURRENT_SENSE_ENABLED` | BTS7960 `IS` pin → resistor → GPIO 36 | Real I²t in the thermal model instead of duty², and **stall detection** |
| `PACK_MONITOR_ENABLED` | Divider from the 12 V pack → GPIO 35 | Motor-pack charge on the dashboard |
| `THERMAL_PROTECTION` | Calibration, not wiring | Assist folds back as the motor heats |

`THERMAL_PROTECTION` is off for a different reason: the constants are
**simulated, not measured**, and an untuned fold-back reducing assist mid-stride
is its own hazard. The load estimate still runs with it off — that is how you
calibrate. Walk the device, watch the peak figure on the dashboard, set
`THERMAL_FULL_DUTY_S` and `THERMAL_COOL_TAU_S` so ordinary use sits well under
0.80, *then* switch it on.

---

## Things the code believes that may not be true

- **The left leg is wired like the right leg.** Copied wholesale. Verify it.
- **The left motor turns the same way as the right.** Almost certainly false
  for a mirrored pair. Set `MOTOR_DIRECTION_SIGN` to `-1` rather than swapping
  motor wires.
- **The IMU is mounted such that gyro X is pitch and gyro Y is roll.** If tilt
  drifts the wrong way, negate the two rates in `readMpu()`.
- **`CURRENT_SENSE_RATIO` is 8500:1.** That is the typical BTS7960 figure.
  Check your own part, and measure the zero-current reading into
  `CURRENT_ZERO_OFFSET_MV`.
- **The dashboard battery is the logic supply.** It cannot represent 12 V at
  all. A sagging motor pack would be invisible until `PACK_MONITOR_ENABLED`.

---

## What is *not* covered by software, at all

- **A physical cut-off switch.** The firmware now drops the H-bridge enables as
  well as the PWM, but that is still software deciding to stop. Add a switch
  you can reach.
- **Mechanical end-stops** limiting joint travel.
- **Wi-Fi loss on the right leg** does not stop the right motor by itself.

---

## About the assist curve

`duty = 80 + 2.5 × |velocity|^1.5`, clamped at 255, reaches the ceiling at
about **17 mm per sensor sample**. With the timing budget pinned at 33 ms that
is roughly 515 mm/s — a brisk leg speed, so the curve is *not* the blunt
instrument it first appears. But it is convex rather than proportional, and its
behaviour still moves with the sensor's sample period.

`ASSIST_CURVE_MODE 1` measures velocity in mm/s against the real sample
interval and responds linearly, which removes both problems. It is **not** the
default, because switching changes what the Sensitivity slider means and needs
re-tuning on the bench. This is an open design decision, not a defect.

---

## Where to start

1. **`docs/safety.md`** — the first-power-up checklist. Motor disconnected for
   step 1. Do not skip to step 5.
2. **`docs/how-it-works.md`** — the control loop and every tunable parameter.
3. **`docs/hardware.md`** — pin map and wiring, including the two dividers that
   are not yet fitted.
4. **The open issues** on GitHub — the `needs-bench-test` label marks everything
   that cannot be closed from a keyboard.

---

## Provenance

The original prototype firmware is **B Dileep Kumar's** work
([@Dileep195](https://github.com/Dileep195)) and is preserved unchanged in
`original/`. The maintained firmware is derived from it. StrideMate was also an
M.Tech project at **IIT Hyderabad**, and institutional policy may apply to some
of this material. `NOTICE.md` records what is still unresolved about that —
read it before assuming you can use this commercially.

The licence is **PolyForm Noncommercial 1.0.0**. Personal, research,
educational, charitable and government use are fine. Commercial use needs a
separate licence.

---

## If you change the control law

Re-run the bench checklist. All of it, motor disconnected first. The failure
mode this device has is a motor driving a human leg the wrong way, and it is
not one you want to discover while wearing it.
