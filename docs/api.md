# HTTP API

All requests are plain HTTP `GET` on port 80.

## Right leg: `10.210.60.121`

These endpoints need the dashboard login (HTTP Basic auth) unless `DASHBOARD_PASSWORD` is empty.

| Endpoint | Description |
|---|---|
| `/` | Dashboard web page |
| `/data` | Live JSON (see below) |
| `/setPWM?value=80..255` | Assist strength (maximum motor duty) |
| `/setSensitivity?value=1..20` | Velocity dead-band |
| `/motor?on=1` / `/motor?on=0` | Enable / disable motors on **both legs** |
| `/stop` | Emergency stop: disable motors on both legs |
| `/toggleMotor` | Flip enabled/disabled (kept from the original firmware) |

Out-of-range values are clamped to the allowed range.

Example `/data` response:

```json
{
  "leftDistance": 212.40, "leftVelocity": -3.10, "leftPWM": 96,
  "leftPitch": 12.50, "leftRoll": -2.10, "leftBattery": 81,
  "rightDistance": 198.70, "rightVelocity": 4.20, "rightPWM": 104,
  "rightPitch": 10.90, "rightRoll": 1.40, "battery": 83,
  "motorEnabled": true, "assistStrength": 255, "sensitivity": 1,
  "leftOnline": true, "tofOk": true, "uptimeMs": 523410
}
```

The first eleven keys have the same names as in the original firmware, so older tools keep working.

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
  "sensitivity": 1, "tofOk": true, "rightLinkOk": true, "uptimeMs": 498120
}
```

> **Security note:** this is a local-network prototype. Passwords travel unencrypted over HTTP, so use a private hotspot with a strong Wi-Fi password.
