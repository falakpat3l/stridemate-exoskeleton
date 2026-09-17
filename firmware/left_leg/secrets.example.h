// =====================================================================
//  secrets.example.h  ->  copy this file to  secrets.h  and fill it in.
//
//  secrets.h is listed in .gitignore, so your real Wi-Fi and passwords
//  are NEVER uploaded to GitHub. Only this example file is public.
// =====================================================================
#pragma once

// Wi-Fi network both ESP32 boards join (e.g. a phone hotspot).
#define WIFI_SSID          "your-wifi-name"
#define WIFI_PASSWORD      "your-wifi-password"

// Password required to flash new firmware over Wi-Fi (Arduino OTA).
// Use the same value on both legs.
#define OTA_PASSWORD       "change-me-ota"

// Shared key the right leg uses when it sends settings (assist strength,
// sensitivity, motor on/off) to the left leg. Must be IDENTICAL on both legs.
#define LINK_KEY           "change-me-link-key"

