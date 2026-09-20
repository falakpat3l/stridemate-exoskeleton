// =====================================================================
//  StrideMate - RIGHT LEG firmware (ESP32)
//
//  What this board does
//   * Reads a VL53L1X time-of-flight sensor (distance, mm) and an MPU6050
//     (tilt) on two separate I2C buses.
//   * Turns the change in distance ("velocity") into a motor assist command
//     for a BTS7960-style H-bridge driving the right-leg motor.
//   * Hosts the Wi-Fi dashboard (both legs, battery, sliders, motor switch).
//   * Polls the LEFT LEG ESP32 for its data and forwards dashboard settings
//     to it, in a background task so a slow/offline left leg never stalls
//     motor control.
//   * Accepts password-protected over-the-air (OTA) firmware updates.
//
//  Board:     ESP32 Dev Module, Arduino-ESP32 core 3.x
//  Libraries: Adafruit VL53L1X (+ its dependency Adafruit BusIO)
//  Setup:     copy secrets.example.h -> secrets.h and fill it in.
//
//  Original prototype firmware by B Dileep Kumar (github.com/Dileep195).
//  See docs/changes-from-original.md for what was changed and why.
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <math.h>
#include "esp_task_wdt.h"
#include "Adafruit_VL53L1X.h"

#include "config.h"
#include "dashboard.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "secrets.h is missing: copy secrets.example.h to secrets.h and fill in your Wi-Fi details."
#endif

// ---------------------------------------------------------------------
//  Hardware objects
// ---------------------------------------------------------------------
TwoWire I2C_TOF = TwoWire(0);
TwoWire I2C_MPU = TwoWire(1);
Adafruit_VL53L1X vl53 = Adafruit_VL53L1X();
WebServer server(80);

// ---------------------------------------------------------------------
//  Right-leg state
// ---------------------------------------------------------------------
float filteredDistance = 0;   // mm, low-pass filtered
float previousDistance = 0;
float velocity         = 0;   // mm per new sensor sample
float pitch = 0, roll = 0;    // degrees, from accelerometer only

bool     tofInitialised  = false;
bool     tofSeeded       = false;   // filter holds a real measurement
uint32_t tofLastSampleMs = 0;       // last reading of any kind (sensor alive)
uint32_t tofLastValidMs  = 0;       // last reading with an actual target
uint32_t tofLastInitTry  = 0;

float smoothedPWM = 0;
int   targetPWM   = 0;
int   lastDirection = 0;      // +1, -1 or 0
bool  bridgeEnabled = false;  // H-bridge enable pins (REN/LEN) state

int batteryPercent = 0;

// ---------------------------------------------------------------------
//  Settings changed from the dashboard (also forwarded to the left leg)
// ---------------------------------------------------------------------
volatile int      assistStrength  = DEFAULT_ASSIST_STRENGTH;
volatile int      sensitivity     = DEFAULT_SENSITIVITY;
volatile bool     motorEnabled    = MOTOR_ENABLED_AT_BOOT;
volatile bool     otaInProgress   = false;
volatile uint32_t settingsVersion = 1;   // bumped on every change

// ---------------------------------------------------------------------
//  Left-leg data (written by the background task, read by the loop)
// ---------------------------------------------------------------------
struct LegTelemetry {
  float distance = 0, velocity = 0, pwm = 0, pitch = 0, roll = 0, battery = 0;
};
LegTelemetry leftLeg;
uint32_t     leftLastOkMs = 0;           // 0 = never received
portMUX_TYPE leftMux = portMUX_INITIALIZER_UNLOCKED;

bool networkStarted = false;

// =====================================================================
//  Small helpers
// =====================================================================
static void writeMotor(int forwardDuty, int reverseDuty) {
  ledcWrite(PIN_MOTOR_RPWM, forwardDuty);
  ledcWrite(PIN_MOTOR_LPWM, reverseDuty);
}

// SAFETY: the H-bridge enable pins are the one path that removes drive
// independently of the PWM peripheral. Previously they were driven HIGH once
// in setup() and never touched, so every "off" in the firmware relied on the
// LEDC channels continuing to output exactly 0%.
static void setBridge(bool on) {
  if (on == bridgeEnabled) return;
  digitalWrite(PIN_MOTOR_REN, on ? HIGH : LOW);
  digitalWrite(PIN_MOTOR_LEN, on ? HIGH : LOW);
  bridgeEnabled = on;
}

static void motorOff() {
  writeMotor(0, 0);
  setBridge(false);           // remove drive entirely; the motor coasts
  smoothedPWM = 0;
  targetPWM = 0;
}

static void bumpSettings() { settingsVersion = settingsVersion + 1; }

// Finds "key": <number> in a flat JSON string. Returns false if not found.
static bool jsonNumber(const String& json, const char* key, float& out) {
  String needle = String("\"") + key + "\"";
  int k = json.indexOf(needle);
  if (k < 0) return false;
  int colon = json.indexOf(':', k + needle.length());
  if (colon < 0) return false;
  out = json.substring(colon + 1).toFloat();   // toFloat stops at ',' or '}'
  return true;
}

static bool leftOnline(uint32_t now) {
  uint32_t last;
  portENTER_CRITICAL(&leftMux);
  last = leftLastOkMs;
  portEXIT_CRITICAL(&leftMux);
  return last != 0 && (now - last) <= LEFT_OFFLINE_AFTER_MS;
}

// The sensor is alive and ranging (it may still be seeing nothing).
static bool tofHealthy(uint32_t now) {
  return tofInitialised && (now - tofLastSampleMs) <= TOF_STALE_TIMEOUT_MS;
}

// The sensor is actually seeing a target. Assist requires this, not just
// a living sensor - otherwise "sensor ok" can be true with no target for
// minutes while the control law runs on a frozen distance.
static bool tofHasTarget(uint32_t now) {
  return tofSeeded && (now - tofLastValidMs) <= TOF_NO_TARGET_TIMEOUT_MS;
}

// =====================================================================
//  Left-leg link (runs on core 0, separate from the control loop)
// =====================================================================
static bool fetchLeftLeg() {
  HTTPClient http;
  http.setConnectTimeout(LEFT_HTTP_TIMEOUT_MS);
  http.setTimeout(LEFT_HTTP_TIMEOUT_MS);
  if (!http.begin(String("http://") + LEFT_LEG_HOST + "/data")) return false;

  bool ok = false;
  if (http.GET() == HTTP_CODE_OK) {
    String payload = http.getString();
    LegTelemetry t;
    if (jsonNumber(payload, "distance", t.distance)) {
      jsonNumber(payload, "velocity", t.velocity);
      jsonNumber(payload, "pwm",      t.pwm);
      jsonNumber(payload, "pitch",    t.pitch);
      jsonNumber(payload, "roll",     t.roll);
      jsonNumber(payload, "battery",  t.battery);
      ok = true;
    } else {
      // Fallback for the original left-leg firmware: first value = distance.
      int c = payload.indexOf(':');
      if (c >= 0) { t.distance = payload.substring(c + 1).toFloat(); ok = true; }
    }
    if (ok) {
      portENTER_CRITICAL(&leftMux);
      leftLeg = t;
      leftLastOkMs = millis();
      if (leftLastOkMs == 0) leftLastOkMs = 1;
      portEXIT_CRITICAL(&leftMux);
    }
  }
  http.end();
  return ok;
}

static bool sendSettingsToLeftLeg() {
  bool motor = motorEnabled && !otaInProgress;
  String url = String("http://") + LEFT_LEG_HOST + "/set?key=" + LINK_KEY +
               "&assist=" + String((int)assistStrength) +
               "&sensitivity=" + String((int)sensitivity) +
               "&motor=" + (motor ? "1" : "0");
  HTTPClient http;
  http.setConnectTimeout(LEFT_HTTP_TIMEOUT_MS);
  http.setTimeout(LEFT_HTTP_TIMEOUT_MS);
  if (!http.begin(url)) return false;
  bool ok = (http.GET() == HTTP_CODE_OK);
  http.end();
  return ok;
}

static void leftLinkTask(void*) {
  uint32_t sentVersion = 0;
  uint32_t lastSettingsSendMs = 0;
  for (;;) {
    uint32_t start = millis();
    if (WiFi.status() == WL_CONNECTED) {
      fetchLeftLeg();
      uint32_t v = settingsVersion;
      // Send immediately on change, and every 2 s anyway (left leg may reboot).
      if (v != sentVersion || millis() - lastSettingsSendMs > 2000) {
        if (sendSettingsToLeftLeg()) {
          sentVersion = v;
          lastSettingsSendMs = millis();
        }
      }
    }
    uint32_t spent = millis() - start;
    vTaskDelay(pdMS_TO_TICKS(spent < LEFT_POLL_PERIOD_MS ? LEFT_POLL_PERIOD_MS - spent : 1));
  }
}

// =====================================================================
//  Sensors
// =====================================================================
static void initTof() {
  tofLastInitTry = millis();
  tofInitialised = false;
  if (!vl53.begin(0x29, &I2C_TOF)) {
    Serial.println("[ToF] VL53L1X not found - check wiring (SDA 25 / SCL 26)");
    return;
  }
  if (TOF_TIMING_BUDGET_MS > 0) vl53.setTimingBudget(TOF_TIMING_BUDGET_MS);
  if (!vl53.startRanging()) {
    Serial.println("[ToF] could not start ranging");
    return;
  }
  tofInitialised  = true;
  tofSeeded       = false;          // force a re-seed on the first reading
  tofLastSampleMs = millis();
  Serial.println("[ToF] VL53L1X ready");
}

static void readTof(uint32_t now) {
  if (!tofInitialised) {
    if (now - tofLastInitTry > 2000) initTof();   // keep retrying
    return;
  }
  if (!vl53.dataReady()) return;                  // no new measurement yet

  int16_t raw = vl53.distance();                  // -1 = invalid / no target
  vl53.clearInterrupt();                          // arm next measurement
  tofLastSampleMs = now;                          // sensor is alive and ranging

  if (raw <= 0 || raw >= TOF_VALID_MAX_MM) {
    velocity = 0;                                 // no target -> never assist
    return;                                       // tofLastValidMs left alone
  }

  // SAFETY: after a gap in VALID readings (target lost to a trouser fold,
  // sunlight, an out-of-range swing, or a sensor re-init), re-seed the filter.
  // Differencing the new reading against a stale one produced a false velocity
  // of a few hundred mm/sample, which saturated the assist curve and commanded
  // near-full power - often in the opposite direction.
  if (!tofSeeded || (now - tofLastValidMs) > TOF_RESEED_AFTER_MS) {
    filteredDistance = raw;
    previousDistance = raw;
    velocity         = 0;
    tofSeeded        = true;
    tofLastValidMs   = now;
    return;
  }

  filteredDistance = DISTANCE_ALPHA * raw + (1.0f - DISTANCE_ALPHA) * filteredDistance;
  velocity = constrain(filteredDistance - previousDistance,
                       -MAX_VELOCITY_MM, MAX_VELOCITY_MM);
  previousDistance = filteredDistance;
  tofLastValidMs   = now;
}

static int16_t read16(TwoWire& bus) {
  // Read high byte THEN low byte (explicit order - see changes doc).
  uint8_t hi = bus.read();
  uint8_t lo = bus.read();
  return (int16_t)((hi << 8) | lo);
}

static void readMpu() {
  I2C_MPU.beginTransmission(MPU_I2C_ADDR);
  I2C_MPU.write(0x3B);                            // ACCEL_XOUT_H
  if (I2C_MPU.endTransmission(false) != 0) return;
  if (I2C_MPU.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)14, true) < 14) return;

  int16_t ax = read16(I2C_MPU);
  int16_t ay = read16(I2C_MPU);
  int16_t az = read16(I2C_MPU);
  read16(I2C_MPU);                                // temperature (unused)
  read16(I2C_MPU);                                // gyro X (unused for now)
  read16(I2C_MPU);                                // gyro Y
  read16(I2C_MPU);                                // gyro Z

  // Same formulas as the original prototype, so dashboard values match.
  pitch = atan2((float)ay, (float)az) * 180.0f / PI;
  roll  = atan2((float)ax, (float)az) * 180.0f / PI;
}

static void initMpu() {
  I2C_MPU.begin(PIN_MPU_SDA, PIN_MPU_SCL, MPU_I2C_FREQ_HZ);
  I2C_MPU.setTimeOut(MPU_I2C_TIMEOUT_MS);         // never block the control loop
  I2C_MPU.beginTransmission(MPU_I2C_ADDR);
  I2C_MPU.write(0x6B);                            // PWR_MGMT_1
  I2C_MPU.write(0);                               // wake up
  if (I2C_MPU.endTransmission(true) != 0) {
    Serial.println("[MPU] MPU6050 not responding - check wiring (SDA 21 / SCL 22)");
  }
}

static void updateBattery() {
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_BATTERY_ADC);
  float volts = (mv / 8.0f / 1000.0f) * BATTERY_DIVIDER_RATIO;
  float pct = (volts - BATTERY_EMPTY_V) * 100.0f / (BATTERY_FULL_V - BATTERY_EMPTY_V);
  batteryPercent = constrain((int)pct, 0, 100);
}

// =====================================================================
//  Assist control
// =====================================================================
static void updateMotor(uint32_t now) {
  bool allowed = motorEnabled && !otaInProgress && tofHealthy(now) && tofHasTarget(now);
  if (STOP_MOTOR_IF_LEFT_OFFLINE && !leftOnline(now)) allowed = false;

  if (!allowed) {                                 // stop immediately, no ramp-down
    motorOff();
    lastDirection = 0;
    return;
  }

  setBridge(true);                                // assist allowed: power the bridge

  int   sens = sensitivity;
  float v    = velocity;
  float absV = fabs(v);
  bool  inRange = filteredDistance >= MIN_DISTANCE_MM && filteredDistance <= MAX_DISTANCE_MM;

  if (inRange && absV > sens) {
    float duty = PWM_MIN_ASSIST + pow(absV, PWM_CURVE_EXPONENT) * PWM_CURVE_GAIN;
    targetPWM = constrain((int)min(duty, 255.0f), PWM_MIN_ASSIST, (int)assistStrength);
  } else {
    targetPWM = 0;
  }

  int direction = ((v > sens) ? 1 : (v < -sens) ? -1 : 0) * MOTOR_DIRECTION_SIGN;

  if (RESET_RAMP_ON_REVERSAL && direction != 0 && lastDirection != 0 && direction != lastDirection) {
    smoothedPWM = 0;                              // brief stop, then ramp up again
    writeMotor(0, 0);
    lastDirection = direction;
    return;
  }
  if (direction != 0) lastDirection = direction;

  smoothedPWM = PWM_ALPHA * targetPWM + (1.0f - PWM_ALPHA) * smoothedPWM;
  smoothedPWM = constrain(smoothedPWM, 0.0f, 255.0f);

  if (direction > 0)      writeMotor((int)smoothedPWM, 0);
  else if (direction < 0) writeMotor(0, (int)smoothedPWM);
  else                    writeMotor(0, 0);
}

// =====================================================================
//  Web server
// =====================================================================
static const char* COLLECTED_HEADERS[] = { "Origin" };

static bool checkAuth() {
  if (strlen(DASHBOARD_PASSWORD) == 0) return true;
  if (server.authenticate(DASHBOARD_USER, DASHBOARD_PASSWORD)) return true;
  server.requestAuthentication();
  return false;
}

// SAFETY: HTTP Basic auth alone does not protect a state change. Once the
// phone has authenticated to this board, any other page open in that browser
// could fire a request at it and the browser would attach the cached
// credentials. CORS does not help - it hides the response, but the request
// still executes, and executing is the whole payload for /motor?on=1.
// State-changing routes are POST (below) and additionally reject a browser
// Origin that is not this device.
static bool sameOrigin() {
  if (!server.hasHeader("Origin")) return true;   // curl and the like
  String expected = String("http://") + WiFi.localIP().toString();
  if (server.header("Origin") == expected) return true;
  server.send(403, "text/plain", "cross-origin request rejected");
  return false;
}

static void handleData() {
  if (!checkAuth()) return;
  uint32_t now = millis();
  LegTelemetry l;
  portENTER_CRITICAL(&leftMux);
  l = leftLeg;
  portEXIT_CRITICAL(&leftMux);

  char json[640];
  snprintf(json, sizeof(json),
    "{\"leftDistance\":%.2f,\"leftVelocity\":%.2f,\"leftPWM\":%.0f,"
    "\"leftPitch\":%.2f,\"leftRoll\":%.2f,\"leftBattery\":%.0f,"
    "\"rightDistance\":%.2f,\"rightVelocity\":%.2f,\"rightPWM\":%d,"
    "\"rightPitch\":%.2f,\"rightRoll\":%.2f,\"battery\":%d,"
    "\"motorEnabled\":%s,\"assistStrength\":%d,\"sensitivity\":%d,"
    "\"leftOnline\":%s,\"tofOk\":%s,\"uptimeMs\":%lu}",
    l.distance, l.velocity, l.pwm, l.pitch, l.roll, l.battery,
    filteredDistance, velocity, (int)smoothedPWM, pitch, roll, batteryPercent,
    motorEnabled ? "true" : "false", (int)assistStrength, (int)sensitivity,
    leftOnline(now) ? "true" : "false", tofHealthy(now) ? "true" : "false",
    (unsigned long)now);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static bool readIntArg(const char* name, int lo, int hi, int& out) {
  if (!server.hasArg(name)) {
    server.send(400, "text/plain", String("missing '") + name + "'");
    return false;
  }
  out = constrain(server.arg(name).toInt(), lo, hi);
  return true;
}

static void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    if (!checkAuth()) return;
    server.send_P(200, "text/html", DASHBOARD_HTML);
  });

  server.on("/data", HTTP_GET, handleData);

  // State-changing routes are POST + same-origin. See sameOrigin() above.
  server.on("/setPWM", HTTP_POST, []() {         // assist strength 80..255
    if (!checkAuth() || !sameOrigin()) return;
    int v;
    if (!readIntArg("value", PWM_MIN_ASSIST, 255, v)) return;
    assistStrength = v;
    bumpSettings();
    server.send(200, "text/plain", "OK");
  });

  server.on("/setSensitivity", HTTP_POST, []() { // dead-band 1..20
    if (!checkAuth() || !sameOrigin()) return;
    int v;
    if (!readIntArg("value", 1, 20, v)) return;
    sensitivity = v;
    bumpSettings();
    server.send(200, "text/plain", "OK");
  });

  server.on("/motor", HTTP_POST, []() {          // explicit on/off: /motor?on=1
    if (!checkAuth() || !sameOrigin()) return;
    int v;
    if (!readIntArg("on", 0, 1, v)) return;
    motorEnabled = (v == 1);
    if (!motorEnabled) motorOff();
    bumpSettings();
    server.send(200, "text/plain", motorEnabled ? "ENABLED" : "DISABLED");
  });

  server.on("/stop", HTTP_POST, []() {           // emergency stop (both legs)
    if (!checkAuth() || !sameOrigin()) return;
    motorEnabled = false;
    motorOff();
    bumpSettings();
    server.send(200, "text/plain", "STOPPED");
  });

  server.on("/toggleMotor", HTTP_POST, []() {    // kept for compatibility
    if (!checkAuth() || !sameOrigin()) return;
    motorEnabled = !motorEnabled;
    if (!motorEnabled) motorOff();
    bumpSettings();
    server.send(200, "text/plain", motorEnabled ? "ENABLED" : "DISABLED");
  });

  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

static void startNetworkServices() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;                         // motors off while flashing
    motorOff();
    bumpSettings();
    Serial.println("[OTA] update started - motor disabled");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaInProgress = false;
    Serial.printf("[OTA] error %u\n", e);
  });
  ArduinoOTA.begin();

  setupRoutes();
  server.collectHeaders(COLLECTED_HEADERS, 1);    // needed by sameOrigin()
  server.begin();
  networkStarted = true;

  Serial.print("[WiFi] connected, dashboard at http://");
  Serial.println(WiFi.localIP());
}

// =====================================================================
//  Setup & loop
// =====================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\nStrideMate right leg starting");

  // Motor driver: PWM channels at zero BEFORE enabling the bridge.
  ledcAttach(PIN_MOTOR_RPWM, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_BITS);
  ledcAttach(PIN_MOTOR_LPWM, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_BITS);
  pinMode(PIN_MOTOR_REN, OUTPUT);
  pinMode(PIN_MOTOR_LEN, OUTPUT);
  digitalWrite(PIN_MOTOR_REN, LOW);   // bridge stays disabled until assist runs
  digitalWrite(PIN_MOTOR_LEN, LOW);
  bridgeEnabled = false;
  motorOff();

  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  initMpu();
  I2C_TOF.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_FREQ_HZ);
  I2C_TOF.setTimeOut(TOF_I2C_TIMEOUT_MS);         // never block the control loop
  initTof();

  // Wi-Fi: static IP, no power-save (lower latency), auto-reconnect.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  IPAddress ip(RIGHT_LEG_IP), gateway(NETWORK_GATEWAY), subnet(NETWORK_SUBNET);
  WiFi.config(ip, gateway, subnet);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("[WiFi] connecting");
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) startNetworkServices();
  else Serial.println("[WiFi] not connected yet - will keep trying in the background");

  updateBattery();

  // SAFETY: watch the control loop. A wedged I2C bus or a stuck handler would
  // otherwise leave the LEDC channels driving the motor at their last duty
  // indefinitely, with no CPU left to switch them off.
  esp_task_wdt_config_t wdtCfg = {
    .timeout_ms     = CONTROL_WDT_TIMEOUT_MS,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  if (esp_task_wdt_init(&wdtCfg) == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdtCfg);            // core 3.x already started it
  }
  esp_task_wdt_add(NULL);                         // subscribe loop()

  // Left-leg link on core 0; Arduino loop() runs on core 1.
  xTaskCreatePinnedToCore(leftLinkTask, "leftLink", 8192, nullptr, 1, nullptr, 0);
}

void loop() {
  uint32_t now = millis();

  if (!networkStarted && WiFi.status() == WL_CONNECTED) startNetworkServices();
  if (networkStarted) {
    ArduinoOTA.handle();
    server.handleClient();
  }

  static uint32_t lastControlMs = 0;
  if (now - lastControlMs >= CONTROL_PERIOD_MS) {
    lastControlMs = now;
    readTof(now);
    readMpu();
    updateMotor(now);
  }

  static uint32_t lastBatteryMs = 0;
  if (now - lastBatteryMs >= 1000) {
    lastBatteryMs = now;
    updateBattery();
  }

  static uint32_t lastPrintMs = 0;
  if (now - lastPrintMs >= SERIAL_PRINT_PERIOD_MS) {
    lastPrintMs = now;
    Serial.printf("Distance: %.1f mm | Velocity: %.1f | TargetPWM: %d | SmoothPWM: %d | Motor: %s%s\n",
                  filteredDistance, velocity, targetPWM, (int)smoothedPWM,
                  motorEnabled ? "on" : "off", tofHealthy(now) ? "" : " | ToF FAULT");
  }

  esp_task_wdt_reset();
  delay(1);
}
