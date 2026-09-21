# Safety

StrideMate is a **research prototype**. It hasn't been clinically validated and isn't a certified medical device. Motors strong enough to move a leg can also injure one. Anyone using this code does so at their own risk (see the [licence](../LICENSE.md)).

## Built-in safety behaviour

| Feature | Right leg | Left leg |
|---|---|---|
| Motor disabled at power-on | ✅ | ✅ |
| **STOP** button on dashboard (stops both legs) | ✅ | ✅ (via link) |
| Motor off if the distance sensor stops reporting for 500 ms | ✅ | ✅ |
| Motor off if the sensor reports but sees **no target** for 500 ms | ✅ | ✅ |
| Distance filter re-seeded after a gap, so a re-acquired target cannot look like a huge movement | ✅ | ✅ |
| Per-sample velocity clamped to `MAX_VELOCITY_MM` | ✅ | ✅ |
| H-bridge enable pins dropped whenever the motor is not allowed (the motor coasts) | ✅ | ✅ |
| I²C buses time out rather than stalling the control loop | ✅ | ✅ |
| Watchdog resets the board if `loop()` stalls for 1 s | ✅ | ✅ |
| Assist folds back on the open-loop motor heat estimate | opt-in (`THERMAL_PROTECTION`, off) | opt-in (`THERMAL_PROTECTION`, off) |
| Stall detection — current drawn while the leg is not moving | opt-in (`CURRENT_SENSE_ENABLED`, off) | opt-in (`CURRENT_SENSE_ENABLED`, off) |
| Motor off while a wireless (OTA) firmware update is flashing | ✅ | ✅ |
| Soft restart when the motor reverses direction | ✅ | ✅ |
| Motor off if the other leg goes silent for 1 s | optional (`STOP_MOTOR_IF_LEFT_OFFLINE`) | ✅ (`STOP_MOTOR_IF_RIGHT_SILENT`) |
| Dashboard, OTA and link protected by passwords | ✅ | ✅ |
| Dashboard inputs range-checked on the board | ✅ | ✅ |

## What is *not* covered by software

- **A physical emergency cut-off** that removes motor power. Add a switch you can reach easily. The firmware now drops the H-bridge enables as well as the PWM, but that is still software deciding to stop.
- **Mechanical end-stops** that limit joint travel to a safe range.
- **Real current or temperature measurement** *until the IS pin is wired*. The firmware side is now ready: set `CURRENT_SENSE_ENABLED` and the thermal model switches from estimating duty² to integrating real I²t, and stall detection starts working. Until then it is an estimate.
- **Stall detection** for the same reason. Once enabled, a motor drawing more than `CURRENT_STALL_A` while the leg has not moved `CURRENT_STALL_MOTION_MM` for `CURRENT_STALL_MS` latches the motor off. The latch clears only when the motor is re-enabled from the dashboard, so it must be acknowledged rather than quietly resetting.
- **Wi-Fi loss on the right leg** doesn't stop the right motor by itself. Use the physical switch.

## Motor heating

The motors are windshield-wiper units: intermittent-duty parts. Nothing on this build measures motor current or temperature, so the firmware keeps an **open-loop estimate** — it integrates duty² and decays it with a 45 s time constant, then folds the assist ceiling back smoothly once the estimate passes 0.80. The dashboard shows the estimate per leg and an **ASSIST REDUCED** badge when fold-back is active.

It folds back rather than cutting out on purpose: losing assist abruptly mid-stride is its own hazard, and reducing duty also reduces heating, so the loop settles.

> **This is off by default** (`THERMAL_PROTECTION false`), because the constants are placeholders from simulation and an untuned fold-back surprising you mid-walk is its own hazard.
>
> The estimate still runs and is still shown on the dashboard with protection off — which is how you calibrate it. Walk the device, watch what load your real use produces, then set `THERMAL_FULL_DUTY_S` and `THERMAL_COOL_TAU_S` so normal use stays well under 0.80 and abuse does not. Only then switch `THERMAL_PROTECTION` on. Better still, wire the BTS7960's `R_IS` / `L_IS` current-sense outputs to a spare ADC pin and close the loop on actual current instead of estimating from duty. Until one of those is done, the motor has no thermal protection at all — keep a hand on the cut-off switch.

## Battery reading

The dashboard's battery figure is configured for a **single Li-ion cell through a 2:1 divider**, and GPIO 34 at 11 dB attenuation can read at most about 6.6 V through that divider. The wiper motors run on 12 V. So the number on the dashboard is the **logic supply, not the motor pack**.

That matters during testing: a sagging motor pack changes the torque a given duty produces, and the dashboard would happily show 100% throughout. Either add a second divider on the motor pack, or read the field as "logic battery" and check the pack separately.

## First power-up checklist

1. Do the first test **with the motor disconnected**. Check in the Serial Monitor that the distance and tilt values look sensible, and that the sensor badge reads *sensor ok* when something is in front of the sensor and *no target* when nothing is.
2. Connect the motor with the exoskeleton **not worn** and the links free to move. Set **Assist Strength** to the minimum (80) and **Sensitivity** high (e.g. 10).
3. Tap **Enable motor** and move the sensor target by hand. The motor should turn in the expected direction. Test **STOP**.
4. Switch off the left leg and confirm the dashboard shows it *offline*. Switch off the right leg and confirm the left motor stops within about 1 s.
5. Only then try it worn, with a supervisor, starting at low assist.

6. Deliberately block the sensor with your hand for a second and uncover it, motor connected but the exoskeleton not worn. The motor should **not** lurch when the target comes back. This is the failure that the filter re-seed fixes; confirm it on the real device.

> The **left-leg firmware is reconstructed** and hasn't been tested on hardware. Its pin map and motor direction are assumptions, so be extra careful with steps 1–3 on that leg. If the left motor drives the wrong way, set `MOTOR_DIRECTION_SIGN` to `-1` in `firmware/left_leg/config.h` rather than swapping the motor wires.
