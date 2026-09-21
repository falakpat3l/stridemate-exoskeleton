# Getting started

This guide assumes no prior Arduino experience.

## 1. Install the software (one time)

1. Download and install the **Arduino IDE 2** from <https://www.arduino.cc/en/software>.
2. Add ESP32 support:
   - Open **Arduino IDE → Settings (Preferences)**.
   - Under *Additional boards manager URLs*, paste:
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   - Open **Tools → Board → Boards Manager**, search **esp32** and install **esp32 by Espressif Systems**, version **3.x**. The firmware uses the 3.x motor-PWM functions (`ledcAttach`), so version 2.x won't compile.
3. Install the sensor library: **Tools → Manage Libraries**, search **Adafruit VL53L1X**, click *Install*, and accept *Install all* when it asks to add **Adafruit BusIO**.

## 2. Get the code

On GitHub, click **Code → Download ZIP** and unzip it, or use `git clone`.

Then read **[../BEFORE-YOU-FLASH-THIS.md](../BEFORE-YOU-FLASH-THIS.md)**. It is
short, and it says which parts of this project have never been tested on real
hardware — worth knowing before you power anything.

## 3. Add your private settings

Do this separately for **both** `firmware/right_leg/` and `firmware/left_leg/`:

1. Make a copy of `secrets.example.h` in the same folder and name it **`secrets.h`**.
2. Open `secrets.h` and fill in:
   - `WIFI_SSID` / `WIFI_PASSWORD`: the Wi-Fi or phone hotspot both boards will join.
   - `OTA_PASSWORD`: needed to update the firmware wirelessly.
   - `LINK_KEY`: any random text. It must be **the same on both legs**.
   - `DASHBOARD_USER` / `DASHBOARD_PASSWORD` (right leg only): your dashboard login.

`secrets.h` is listed in `.gitignore`, so git will never upload it.

## 4. Check the network settings

Both boards use **fixed IP addresses** (set in `config.h`):

| Board | IP address |
|---|---|
| Right leg (dashboard) | `10.210.60.121` |
| Left leg | `10.210.60.122` |
| Gateway | `10.210.60.1` |

These have to match your hotspot or router. If your phone hotspot hands out addresses like `192.168.43.x`, change `RIGHT_LEG_IP`, `LEFT_LEG_IP`, `LEFT_LEG_HOST` and `NETWORK_GATEWAY` in **both** `config.h` files to match.

## 5. Upload the firmware (USB)

1. Plug the **right-leg** ESP32 into your computer with a USB data cable.
2. Open `firmware/right_leg/right_leg.ino`. The other files open as tabs.
3. Choose **Tools → Board → esp32 → ESP32 Dev Module**.
4. *(Recommended)* Choose **Tools → Partition Scheme → Minimal SPIFFS (1.9MB APP with OTA)**. The default scheme works too, but the firmware already fills about 90% of it.
5. Choose **Tools → Port** and pick the board's port.
6. Click **Upload** (→).
7. Open **Tools → Serial Monitor** at **115200** baud. You should see `[ToF] VL53L1X ready` and `[WiFi] connected`.
8. Repeat with the **left-leg** board and `firmware/left_leg/left_leg.ino`.

## 6. Open the dashboard

1. Connect your phone to the same Wi-Fi or hotspot.
2. Open **http://10.210.60.121/** and log in.
3. Check that the **LEFT LEG** badge shows *online* and the right leg shows *sensor ok*.
4. The motor starts **disabled**. Follow [safety.md](safety.md) before tapping **Enable motor**.

## 7. Wireless (OTA) updates, optional

Once a board is on Wi-Fi, it shows up under **Tools → Port** as `stridemate-right` or `stridemate-left`. Select it and upload as usual. The IDE will ask for the `OTA_PASSWORD`. The motors switch off automatically while an update is flashing.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| `secrets.h is missing` when compiling | Step 3 wasn't done for that folder |
| `ledcAttach was not declared` | ESP32 board package is 2.x. Update to 3.x |
| `[ToF] VL53L1X not found` | Check the sensor wiring to pins 25 (SDA) and 26 (SCL), and 3.3 V/GND |
| `[MPU] MPU6050 not responding` | Check the wiring to pins 21 (SDA) and 22 (SCL) |
| Serial Monitor shows dots forever | Wrong Wi-Fi name/password, or the IP settings don't match the network |
| Dashboard shows LEFT LEG *offline* | Left board is off, on another network, or has a different IP |
| Motor never moves | Motor not enabled on the dashboard, sensor fault, no target in view, distance outside 50–450 mm, or movement below the sensitivity threshold |
| Red **STALL** banner, motor dead | A stall was detected and the motor is latched off. Clear the obstruction, then tap **Enable motor** — re-enabling is what clears the latch |
| Battery or current reads *not wired* | Normal. Those sensors ship disabled; see `PACK_MONITOR_ENABLED` and `CURRENT_SENSE_ENABLED` |
