# Safety

StrideMate is a **research prototype**. It hasn't been clinically validated and isn't a certified medical device. Motors strong enough to move a leg can also injure one. Anyone using this code does so at their own risk (see the [license](../LICENSE)).

## Built-in safety behaviour

| Feature | Right leg | Left leg |
|---|---|---|
| Motor disabled at power-on | ✅ | ✅ |
| **STOP** button on dashboard (stops both legs) | ✅ | ✅ (via link) |
| Motor off if the distance sensor stops reporting for 500 ms | ✅ | ✅ |
| Motor off while a wireless (OTA) firmware update is flashing | ✅ | ✅ |
| Soft restart when the motor reverses direction | ✅ | ✅ |
| Motor off if the other leg goes silent for 1 s | optional (`STOP_MOTOR_IF_LEFT_OFFLINE`) | ✅ (`STOP_MOTOR_IF_RIGHT_SILENT`) |
| Dashboard, OTA and link protected by passwords | ✅ | ✅ |
| Dashboard inputs range-checked on the board | ✅ | ✅ |

## What is *not* covered by software

- **A physical emergency cut-off** that removes motor power. Add a switch you can reach easily.
- **Mechanical end-stops** that limit joint travel to a safe range.
- **Current or stall detection.** A blocked motor isn't detected.
- **Wi-Fi loss on the right leg** doesn't stop the right motor by itself. Use the physical switch.

## First power-up checklist

1. Do the first test **with the motor disconnected**. Check in the Serial Monitor that the distance and tilt values look sensible.
2. Connect the motor with the exoskeleton **not worn** and the links free to move. Set **Assist Strength** to the minimum (80) and **Sensitivity** high (e.g. 10).
3. Tap **Enable motor** and move the sensor target by hand. The motor should turn in the expected direction. Test **STOP**.
4. Switch off the left leg and confirm the dashboard shows it *offline*. Switch off the right leg and confirm the left motor stops within about 1 s.
5. Only then try it worn, with a supervisor, starting at low assist.

> The **left-leg firmware is reconstructed** and hasn't been tested on hardware. Its pin map and motor direction are assumptions, so be extra careful with steps 1–3 on that leg.
