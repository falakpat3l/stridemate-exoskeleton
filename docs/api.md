# HTTP API

Port 80, plain HTTP. **Reads are `GET`; anything that changes state is `POST`** and is additionally rejected if the browser sends an `Origin` header that is not this device.

Why: HTTP Basic auth alone does not protect a state change. Once a phone has authenticated to the board, any other page open in that browser could have fired `<img src="http://10.210.60.121/motor?on=1">` and the browser would have attached the cached credentials. CORS does not help — it hides the response, but the request still executes. Clients that send no `Origin` header (curl, scripts) are unaffected.

## Right leg: `10.210.60.121`

These endpoints need the dashboard login (HTTP Basic auth) unless `DASHBOARD_PASSWORD` is empty.

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Dashboard web page |
| `/data` | GET | Live JSON (see below) |
| `/setPWM?value=80..255` | **POST** | Assist strength (maximum motor duty) |
| `/setSensitivity?value=1..20` | **POST** | Velocity dead-band |
| `/motor?on=1` / `/motor?on=0` | **POST** | Enable / disable motors on **both legs** |
| `/stop` | **POST** | Emergency stop: disable motors on both legs |
| `/toggleMotor` | **POST** | Flip enabled/disabled (kept from the original firmware) |

Out-of-range values are clamped to the allowed range.

Example `/data` response:

```json
{
  "leftDistance": 212.40, "leftVelocity": -3.10, "leftPWM": 96,
  "leftPitch": 12.50, "leftRoll": -2.10, "leftBattery": 81, "leftThermal": 0.14,
  "rightDistance": 198.70, "rightVelocity": 4.20, "rightPWM": 104,
  "rightPitch": 10.90, "rightRoll": 1.40, "battery": 83,
  "motorEnabled": true, "assistStrength": 255, "sensitivity": 1,
  "leftOnline": true, "tofOk": true, "tofTarget": true, "thermal": 0.22,
  "uptimeMs": 523410
}
```

The original firmware's key names are unchanged, so older tools keep working. Added since:

| Key | Meaning |
|---|---|
| `tofOk` | The sensor is alive and ranging |
| `tofTarget` | The sensor is actually seeing something. **Assist requires both.** A live sensor with no target used to be shown as healthy |
| `thermal` / `leftThermal` | Motor heat estimate, 0–1. Assist folds back above 0.80 |
| `thermalPeak` | Highest load seen since boot — the number to read when calibrating the thermal constants |
| `stall` | A stall was detected and the motor is latched off. Always `false` unless `CURRENT_SENSE_ENABLED` |
| `packPercent` | Motor-pack charge, or `-1` when `PACK_MONITOR_ENABLED` is off |
| `currentA` | Measured motor current, `0` unless `CURRENT_SENSE_ENABLED` |

## Left leg: `10.210.60.122`

| Endpoint | Description |
|---|---|
| `/` | Short text note pointing to the right-leg dashboard |
| `/data` | Live JSON (no login, read-only) |
| `/set?key=LINK_KEY&assist=..&sensitivity=..&motor=0\|1` | Settings from the right leg. Returns `403` if the key is wrong |

Example `/data` response:

```json
{
  "distance": 212.40, "velocity": -3.10, "pwm": 96, "pitch": 12.50,
  "roll": -2.10, "battery": 81, "motorEnabled": true, "assistStrength": 255,
  "sensitivity": 1, "tofOk": true, "tofTarget": true, "thermal": 0.18,
  "rightLinkOk": true, "uptimeMs": 498120
}
```

> **Security note:** this is a local-network prototype. Passwords travel unencrypted over HTTP, and `LINK_KEY` travels in the query string of every `/set`, so it will appear in any intermediate log. Use a private hotspot with a strong Wi-Fi password, and do not expose these boards to the internet.

> **If you proxy the dashboard** (a tunnel, a reverse proxy, anything that changes the hostname the browser sees), the `Origin` check will reject the control endpoints, because the origin no longer matches the board's own IP. Relax `sameOrigin()` deliberately in that case — do not disable it by accident.
