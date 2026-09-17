// =====================================================================
//  StrideMate - LEFT LEG firmware (ESP32)
//
//  !!!  RECONSTRUCTED - NOT YET TESTED ON THE REAL HARDWARE  !!!
//
//  The original left-leg code was not available. This version was written
//  to match the right-leg firmware and what the right leg expects from it:
//   * same sensors, pins and assist algorithm as the right leg (ASSUMED -
//     check the wiring in config.h against the physical left leg);
//   * serves GET /data as JSON for the right leg / dashboard;
//   * accepts GET /set?key=..&assist=..&sensitivity=..&motor=.. from the
//     right leg, so dashboard sliders and STOP apply to both legs;
//   * stops its motor if the right leg goes silent (safety).
//
//  Test with the motor mechanically disconnected first. See docs/safety.md.
//
//  Board:     ESP32 Dev Module, Arduino-ESP32 core 3.x
//  Libraries: Adafruit VL53L1X (+ Adafruit BusIO)
//  Setup:     copy secrets.example.h -> secrets.h (same values as right leg).
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <math.h>
#include "Adafruit_VL53L1X.h"

#include "config.h"

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
//  Left-leg state
// ---------------------------------------------------------------------
float filteredDistance = 0;   // mm, low-pass filtered
float previousDistance = 0;
float velocity         = 0;   // mm per new sensor sample
float pitch = 0, roll = 0;    // degrees, from accelerometer only

bool     tofInitialised  = false;
uint32_t tofLastSampleMs = 0;
uint32_t tofLastInitTry  = 0;

float smoothedPWM = 0;
int   targetPWM   = 0;
int   lastDirection = 0;      // +1, -1 or 0

int batteryPercent = 0;

// Settings received from the right leg
int  assistStrength = DEFAULT_ASSIST_STRENGTH;
int  sensitivity    = DEFAULT_SENSITIVITY;
bool motorEnabled   = MOTOR_ENABLED_AT_BOOT;
bool otaInProgress  = false;

uint32_t rightLastHeardMs = 0;   // 0 = never
bool networkStarted = false;

// =====================================================================
//  Small helpers
// =====================================================================
static void writeMotor(int forwardDuty, int reverseDuty) {
  ledcWrite(PIN_MOTOR_RPWM, forwardDuty);
  ledcWrite(PIN_MOTOR_LPWM, reverseDuty);
}

static void motorOff() {
  writeMotor(0, 0);
  smoothedPWM = 0;
  targetPWM = 0;
}

static bool tofHealthy(uint32_t now) {
  return tofInitialised && (now - tofLastSampleMs) <= TOF_STALE_TIMEOUT_MS;
}

static bool rightLegHeard(uint32_t now) {
  return rightLastHeardMs != 0 && (now - rightLastHeardMs) <= RIGHT_SILENT_TIMEOUT_MS;
}

// Any request coming from the right leg's IP counts as a heartbeat.
static void noteRequestFrom() {
  IPAddress rightIp(RIGHT_LEG_IP);
  if (server.client().remoteIP() == rightIp) {
    rightLastHeardMs = millis();
    if (rightLastHeardMs == 0) rightLastHeardMs = 1;
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
  tofLastSampleMs = now;

  if (raw > 0 && raw < TOF_VALID_MAX_MM) {
    filteredDistance = DISTANCE_ALPHA * raw + (1.0f - DISTANCE_ALPHA) * filteredDistance;
  }
  velocity = filteredDistance - previousDistance;
  previousDistance = filteredDistance;
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
  bool allowed = motorEnabled && !otaInProgress && tofHealthy(now);
  if (STOP_MOTOR_IF_RIGHT_SILENT && !rightLegHeard(now)) allowed = false;

  if (!allowed) {                                 // stop immediately, no ramp-down
    motorOff();
    lastDirection = 0;
    return;
  }

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

  int direction = (v > sens) ? 1 : (v < -sens) ? -1 : 0;

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
static void handleData() {
  noteRequestFrom();
  uint32_t now = millis();
  char json[400];
  snprintf(json, sizeof(json),
    "{\"distance\":%.2f,\"velocity\":%.2f,\"pwm\":%d,\"pitch\":%.2f,"
    "\"roll\":%.2f,\"battery\":%d,\"motorEnabled\":%s,\"assistStrength\":%d,"
    "\"sensitivity\":%d,\"tofOk\":%s,\"rightLinkOk\":%s,\"uptimeMs\":%lu}",
    filteredDistance, velocity, (int)smoothedPWM, pitch, roll, batteryPercent,
    motorEnabled ? "true" : "false", assistStrength, sensitivity,
    tofHealthy(now) ? "true" : "false", rightLegHeard(now) ? "true" : "false",
    (unsigned long)now);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", json);
}

static void handleSet() {
  if (server.arg("key") != LINK_KEY) {
    server.send(403, "text/plain", "bad key");
    return;
  }
  noteRequestFrom();
  if (server.hasArg("assist"))
    assistStrength = constrain(server.arg("assist").toInt(), PWM_MIN_ASSIST, 255);
  if (server.hasArg("sensitivity"))
    sensitivity = constrain(server.arg("sensitivity").toInt(), 1, 20);
  if (server.hasArg("motor")) {
    motorEnabled = server.arg("motor") == "1";
    if (!motorEnabled) motorOff();
  }
  server.send(200, "text/plain", "OK");
}

static void setupRoutes() {
  server.on("/", HTTP_GET, []() {
    server.send(200, "text/plain",
      "StrideMate left leg. Open the dashboard on the right leg (http://10.210.60.121/).\n"
      "Raw data: /data");
  });
  server.on("/data", HTTP_GET, handleData);
  server.on("/set", HTTP_GET, handleSet);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
}

static void startNetworkServices() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    otaInProgress = true;                         // motors off while flashing
    motorOff();
    Serial.println("[OTA] update started - motor disabled");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    otaInProgress = false;
    Serial.printf("[OTA] error %u\n", e);
  });
  ArduinoOTA.begin();

  setupRoutes();
  server.begin();
  networkStarted = true;

  Serial.print("[WiFi] connected, left leg at http://");
  Serial.println(WiFi.localIP());
}

// =====================================================================
//  Setup & loop
// =====================================================================
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(200);
  Serial.println("\nStrideMate LEFT leg starting (reconstructed firmware)");

  // Motor driver: PWM channels at zero BEFORE enabling the bridge.
  ledcAttach(PIN_MOTOR_RPWM, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_BITS);
  ledcAttach(PIN_MOTOR_LPWM, MOTOR_PWM_FREQ_HZ, MOTOR_PWM_BITS);
  motorOff();
  pinMode(PIN_MOTOR_REN, OUTPUT);
  pinMode(PIN_MOTOR_LEN, OUTPUT);
  digitalWrite(PIN_MOTOR_REN, HIGH);
  digitalWrite(PIN_MOTOR_LEN, HIGH);

  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);

  initMpu();
  I2C_TOF.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_FREQ_HZ);
  initTof();

  // Wi-Fi: static IP, no power-save (lower latency), auto-reconnect.
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  IPAddress ip(LEFT_LEG_IP), gateway(NETWORK_GATEWAY), subnet(NETWORK_SUBNET);
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
    Serial.printf("[L] Distance: %.1f mm | Velocity: %.1f | TargetPWM: %d | SmoothPWM: %d | Motor: %s%s\n",
                  filteredDistance, velocity, targetPWM, (int)smoothedPWM,
                  motorEnabled ? "on" : "off", tofHealthy(now) ? "" : " | ToF FAULT");
    if (STOP_MOTOR_IF_RIGHT_SILENT && !rightLegHeard(now)) Serial.println("[L] right leg silent - motor held off");
  }

  delay(1);
}
