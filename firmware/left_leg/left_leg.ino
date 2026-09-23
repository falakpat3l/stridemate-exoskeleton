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
#include "esp_task_wdt.h"
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
float velocity         = 0;   // mm per new sensor sample (ASSIST_CURVE_MODE 0)
float velocityMmS      = 0;   // mm per second          (ASSIST_CURVE_MODE 1)
float pitch = 0, roll = 0;    // degrees

bool     tofInitialised  = false;
bool     tofSeeded       = false;   // filter holds a real measurement
uint32_t tofLastSampleMs = 0;       // last reading of any kind (sensor alive)
uint32_t tofLastValidMs  = 0;       // last reading with an actual target
uint32_t tofLastInitTry  = 0;

float smoothedPWM = 0;
int   targetPWM   = 0;
int   lastDirection = 0;      // +1, -1 or 0
bool  bridgeEnabled = false;  // H-bridge enable pins (REN/LEN) state
float thermalLoad   = 0;      // heat estimate, 0..1.5 (1.0 = the budget)
float thermalPeak   = 0;      // highest load seen since boot - calibration aid
float motorCurrentA = 0;      // 0 unless CURRENT_SENSE_ENABLED

// SAFETY: latched when the motor draws current without the leg moving.
// Cleared only by re-enabling the motor, so it is acknowledged deliberately.
bool     stallFault      = false;
uint32_t stallSinceMs    = 0;
float    stallRefDistance = 0;

float packVolts   = 0;        // motor pack, 0 unless PACK_MONITOR_ENABLED
int   packPercent = -1;       // -1 = not monitored

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
    velocity    = 0;                              // no target -> never assist
    velocityMmS = 0;
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
    velocityMmS      = 0;
    tofSeeded        = true;
    tofLastValidMs   = now;
    return;
  }

  // The REAL interval between valid samples, not the configured one. This is
  // what makes ASSIST_CURVE_MODE 1 independent of TOF_TIMING_BUDGET_MS.
  const float sampleDtS = (now - tofLastValidMs) / 1000.0f;

  filteredDistance = DISTANCE_ALPHA * raw + (1.0f - DISTANCE_ALPHA) * filteredDistance;
  velocity = constrain(filteredDistance - previousDistance,
                       -MAX_VELOCITY_MM, MAX_VELOCITY_MM);
  velocityMmS = (sampleDtS > 1e-4f)
                  ? constrain(velocity / sampleDtS, -MAX_VELOCITY_MM_S, MAX_VELOCITY_MM_S)
                  : 0.0f;
  previousDistance = filteredDistance;
  tofLastValidMs   = now;
}

static int16_t read16(TwoWire& bus) {
  // Read high byte THEN low byte (explicit order - see changes doc).
  uint8_t hi = bus.read();
  uint8_t lo = bus.read();
  return (int16_t)((hi << 8) | lo);
}

static void readMpu(float dtS) {
  I2C_MPU.beginTransmission(MPU_I2C_ADDR);
  I2C_MPU.write(0x3B);                            // ACCEL_XOUT_H
  if (I2C_MPU.endTransmission(false) != 0) return;
  if (I2C_MPU.requestFrom((uint8_t)MPU_I2C_ADDR, (uint8_t)14, true) < 14) return;

  int16_t ax = read16(I2C_MPU);
  int16_t ay = read16(I2C_MPU);
  int16_t az = read16(I2C_MPU);
  read16(I2C_MPU);                                // temperature (unused)
  int16_t gx = read16(I2C_MPU);                   // gyro X
  int16_t gy = read16(I2C_MPU);                   // gyro Y
  read16(I2C_MPU);                                // gyro Z (yaw, unused)

  // Accelerometer-only tilt: the original prototype's formulas. Accurate at
  // rest, noisy under the accelerations of walking - which is exactly when
  // the number is being looked at.
  const float accPitch = atan2((float)ay, (float)az) * 180.0f / PI;
  const float accRoll  = atan2((float)ax, (float)az) * 180.0f / PI;

  if (USE_GYRO_FUSION && dtS > 0.0f && dtS < 0.5f) {
    // Complementary filter: trust the gyro over short intervals, and let the
    // accelerometer pull it back so integration drift cannot accumulate.
    // If tilt moves the wrong way on your build, negate these two rates -
    // the sign depends on how the MPU6050 is mounted.
    const float ratePitch = (float)gx / MPU_GYRO_LSB_PER_DPS;   // deg/s
    const float rateRoll  = (float)gy / MPU_GYRO_LSB_PER_DPS;
    pitch = GYRO_FUSION_ALPHA * (pitch + ratePitch * dtS)
          + (1.0f - GYRO_FUSION_ALPHA) * accPitch;
    roll  = GYRO_FUSION_ALPHA * (roll + rateRoll * dtS)
          + (1.0f - GYRO_FUSION_ALPHA) * accRoll;
  } else {
    pitch = accPitch;
    roll  = accRoll;
  }
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

// The 12 V motor pack, on its own divider. Off until one is wired: see
// PACK_MONITOR_ENABLED in config.h.
static void updatePack() {
  if (!PACK_MONITOR_ENABLED) { packVolts = 0; packPercent = -1; return; }
  uint32_t mv = 0;
  for (int i = 0; i < 8; i++) mv += analogReadMilliVolts(PIN_PACK_ADC);
  packVolts = (mv / 8.0f / 1000.0f) * PACK_DIVIDER_RATIO;
  float pct = (packVolts - PACK_EMPTY_V) * 100.0f / (PACK_FULL_V - PACK_EMPTY_V);
  packPercent = constrain((int)pct, 0, 100);
}

// Motor current from the BTS7960's IS pin. Off until wired: see
// CURRENT_SENSE_ENABLED in config.h. Read every control cycle, because both
// stall detection and the thermal model want it fresh.
static void updateCurrent() {
  if (!CURRENT_SENSE_ENABLED) { motorCurrentA = 0; return; }
  const float mv = (float)analogReadMilliVolts(PIN_CURRENT_ADC) - CURRENT_ZERO_OFFSET_MV;
  const float senseA = (mv / 1000.0f) / CURRENT_SENSE_RESISTOR_OHMS;
  motorCurrentA = max(0.0f, senseA * CURRENT_SENSE_RATIO);
}


// =====================================================================
//  Motor thermal budget (open loop - no current sensor on this build)
// =====================================================================
static void updateThermal(float duty, float dtS) {
  // dtS is MEASURED, not assumed. The control loop runs at best effort, so
  // whenever the loop is busy the real interval is longer than
  // CONTROL_PERIOD_MS and a fixed dt makes the estimate read low.
  if (!(dtS > 0.0f) || dtS > 1.0f) dtS = CONTROL_PERIOD_MS / 1000.0f;

  // With a current sensor this is real I^2t. Without one it is duty^2, an
  // estimate - which is why THERMAL_PROTECTION ships disabled.
  const float f = CURRENT_SENSE_ENABLED ? (motorCurrentA / MOTOR_CURRENT_FULL_A)
                                        : (duty / 255.0f);

  thermalLoad += (f * f / THERMAL_FULL_DUTY_S - thermalLoad / THERMAL_COOL_TAU_S) * dtS;
  thermalLoad = constrain(thermalLoad, 0.0f, 1.5f);
  if (thermalLoad > thermalPeak) thermalPeak = thermalLoad;   // calibration aid
}

// 1.0 = full assist available, falling smoothly to THERMAL_MIN_ASSIST_FRAC.
static float thermalHeadroom() {
  if (!THERMAL_PROTECTION || thermalLoad <= THERMAL_WARN_LOAD) return 1.0f;
  float f = 1.0f - (thermalLoad - THERMAL_WARN_LOAD) / (1.0f - THERMAL_WARN_LOAD);
  return constrain(f, THERMAL_MIN_ASSIST_FRAC, 1.0f);
}

// =====================================================================
//  Assist control
// =====================================================================
static void updateMotor(uint32_t now, float dtS) {
  bool allowed = motorEnabled && !otaInProgress && tofHealthy(now) && tofHasTarget(now)
                 && !stallFault;
  if (STOP_MOTOR_IF_RIGHT_SILENT && !rightLegHeard(now)) allowed = false;

  if (!allowed) {                                 // stop immediately, no ramp-down
    motorOff();
    lastDirection = 0;
    stallSinceMs  = 0;
    updateThermal(0, dtS);                        // keep cooling while off
    return;
  }

  setBridge(true);                                // assist allowed: power the bridge

  const int sens = sensitivity;

#if ASSIST_CURVE_MODE == 0
  const float vSigned = velocity;                 // mm per sensor sample
  const float dead    = (float)sens;
#else
  const float vSigned = velocityMmS;              // mm per second
  const float dead    = (float)sens * SENSITIVITY_MMS_PER_STEP;
#endif
  const float absV    = fabs(vSigned);
  const bool  inRange = filteredDistance >= MIN_DISTANCE_MM &&
                        filteredDistance <= MAX_DISTANCE_MM;

  if (inRange && absV > dead) {
#if ASSIST_CURVE_MODE == 0
    float duty = PWM_MIN_ASSIST + pow(absV, PWM_CURVE_EXPONENT) * PWM_CURVE_GAIN;
#else
    // Rises across the whole speed range instead of saturating at about
    // 17 mm/sample, which is what makes mode 0 effectively bang-bang.
    const float span = constrain((absV - dead) /
                                 max(1.0f, ASSIST_SPEED_FULL_MMS - dead), 0.0f, 1.0f);
    float duty = PWM_MIN_ASSIST +
                 (float)((int)assistStrength - PWM_MIN_ASSIST) *
                 pow(span, ASSIST_CURVE_EXPONENT_P);
#endif
    targetPWM = constrain((int)min(duty, 255.0f), PWM_MIN_ASSIST, (int)assistStrength);
  } else {
    targetPWM = 0;
  }

  // THERMAL: cap the assist ceiling by the remaining heat budget.
  int maxDuty = PWM_MIN_ASSIST +
                (int)((assistStrength - PWM_MIN_ASSIST) * thermalHeadroom());
  if (targetPWM > maxDuty) targetPWM = maxDuty;

  int direction = ((vSigned > dead) ? 1 : (vSigned < -dead) ? -1 : 0) * MOTOR_DIRECTION_SIGN;

  if (RESET_RAMP_ON_REVERSAL && direction != 0 && lastDirection != 0 && direction != lastDirection) {
    smoothedPWM = 0;                              // brief stop, then ramp up again
    writeMotor(0, 0);
    lastDirection = direction;
    updateThermal(0, dtS);
    return;
  }
  if (direction != 0) lastDirection = direction;

  smoothedPWM = PWM_ALPHA * targetPWM + (1.0f - PWM_ALPHA) * smoothedPWM;
  smoothedPWM = constrain(smoothedPWM, 0.0f, 255.0f);

  if (direction > 0)      writeMotor((int)smoothedPWM, 0);
  else if (direction < 0) writeMotor(0, (int)smoothedPWM);
  else                    writeMotor(0, 0);

  updateThermal(direction != 0 ? smoothedPWM : 0, dtS);

  // SAFETY: a stall is current flowing while the leg is not actually moving.
  // Inert until CURRENT_SENSE_ENABLED - without a sensor there is nothing to
  // detect it with, which is the whole point of wiring the IS pin.
  if (CURRENT_SENSE_ENABLED && motorCurrentA > CURRENT_STALL_A && smoothedPWM > 0) {
    if (stallSinceMs == 0) {
      stallSinceMs     = now;
      stallRefDistance = filteredDistance;
    } else if ((now - stallSinceMs) >= CURRENT_STALL_MS &&
               fabs(filteredDistance - stallRefDistance) < CURRENT_STALL_MOTION_MM) {
      stallFault = true;                          // latched until re-enabled
      motorOff();
      Serial.println("[STALL] current high with no movement - motor latched off");
    }
  } else {
    stallSinceMs = 0;
  }
}

// =====================================================================
//  Web server
// =====================================================================
static void handleData() {
  noteRequestFrom();
  uint32_t now = millis();
  char json[576];
  snprintf(json, sizeof(json),
    "{\"distance\":%.2f,\"velocity\":%.2f,\"pwm\":%d,\"pitch\":%.2f,"
    "\"roll\":%.2f,\"battery\":%d,\"motorEnabled\":%s,\"assistStrength\":%d,"
    "\"sensitivity\":%d,\"tofOk\":%s,\"tofTarget\":%s,\"thermal\":%.2f,"
    "\"thermalPeak\":%.2f,\"stall\":%s,\"packPercent\":%d,\"currentA\":%.2f,"
    "\"rightLinkOk\":%s,\"uptimeMs\":%lu}",
    filteredDistance, velocity, (int)smoothedPWM, pitch, roll, batteryPercent,
    motorEnabled ? "true" : "false", assistStrength, sensitivity,
    tofHealthy(now) ? "true" : "false", tofHasTarget(now) ? "true" : "false",
    thermalLoad, thermalPeak, stallFault ? "true" : "false", packPercent,
    motorCurrentA, rightLegHeard(now) ? "true" : "false",
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
    if (motorEnabled) { stallFault = false; stallSinceMs = 0; }
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
    // The upload runs inside ArduinoOTA.handle() for several seconds and
    // never feeds the task watchdog, so the 1 s control-loop watchdog
    // rebooted the board part-way through every wireless update. The
    // watchdog guards against the motor being driven by a wedged loop;
    // with the H-bridge disabled above there is nothing to guard, so
    // unsubscribe for the upload. Success reboots; failure re-subscribes.
    esp_task_wdt_delete(NULL);
    Serial.println("[OTA] update started - motor disabled");
  });
  ArduinoOTA.onError([](ota_error_t e) {
    esp_task_wdt_add(NULL);                       // watch the loop again
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
  pinMode(PIN_MOTOR_REN, OUTPUT);
  pinMode(PIN_MOTOR_LEN, OUTPUT);
  digitalWrite(PIN_MOTOR_REN, LOW);   // bridge stays disabled until assist runs
  digitalWrite(PIN_MOTOR_LEN, LOW);
  bridgeEnabled = false;
  motorOff();

  analogSetPinAttenuation(PIN_BATTERY_ADC, ADC_11db);
  if (PACK_MONITOR_ENABLED)  analogSetPinAttenuation(PIN_PACK_ADC, ADC_11db);
  if (CURRENT_SENSE_ENABLED) analogSetPinAttenuation(PIN_CURRENT_ADC, ADC_11db);

  initMpu();
  I2C_TOF.begin(PIN_TOF_SDA, PIN_TOF_SCL, TOF_I2C_FREQ_HZ);
  I2C_TOF.setTimeOut(TOF_I2C_TIMEOUT_MS);         // never block the control loop
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
  updatePack();

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
    // Measured, not assumed - this loop also serves HTTP and OTA.
    const float dtS = (lastControlMs == 0) ? (CONTROL_PERIOD_MS / 1000.0f)
                                           : (now - lastControlMs) / 1000.0f;
    lastControlMs = now;
    readTof(now);
    readMpu(dtS);
    updateCurrent();
    updateMotor(now, dtS);
  }

  static uint32_t lastBatteryMs = 0;
  if (now - lastBatteryMs >= 1000) {
    lastBatteryMs = now;
    updateBattery();
    updatePack();
  }

  static uint32_t lastPrintMs = 0;
  if (now - lastPrintMs >= SERIAL_PRINT_PERIOD_MS) {
    lastPrintMs = now;
    Serial.printf("[L] D %.0fmm | v %.0fmm/s (%.1f/sample) | tgt %d | pwm %d | "
                  "load %.2f peak %.2f%s | %s%s%s\n",
                  filteredDistance, velocityMmS, velocity, targetPWM, (int)smoothedPWM,
                  thermalLoad, thermalPeak,
                  CURRENT_SENSE_ENABLED ? "" : " (est)",
                  motorEnabled ? "on" : "off",
                  tofHealthy(now) ? "" : " | ToF FAULT",
                  stallFault ? " | STALL" : "");
    if (STOP_MOTOR_IF_RIGHT_SILENT && !rightLegHeard(now)) Serial.println("[L] right leg silent - motor held off");
  }

  esp_task_wdt_reset();
  delay(1);
}
