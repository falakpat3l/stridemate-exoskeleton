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
  float distance = 0, velocity = 0, pwm = 0, pitch = 0, roll = 0, battery = 0, thermal = 0;
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
      jsonNumber(payload, "thermal",  t.thermal);
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
  if (STOP_MOTOR_IF_LEFT_OFFLINE && !leftOnline(now)) allowed = false;

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

  char json[896];
  snprintf(json, sizeof(json),
    "{\"leftDistance\":%.2f,\"leftVelocity\":%.2f,\"leftPWM\":%.0f,"
    "\"leftPitch\":%.2f,\"leftRoll\":%.2f,\"leftBattery\":%.0f,\"leftThermal\":%.2f,"
    "\"rightDistance\":%.2f,\"rightVelocity\":%.2f,\"rightPWM\":%d,"
    "\"rightPitch\":%.2f,\"rightRoll\":%.2f,\"battery\":%d,"
    "\"motorEnabled\":%s,\"assistStrength\":%d,\"sensitivity\":%d,"
    "\"leftOnline\":%s,\"tofOk\":%s,\"tofTarget\":%s,\"thermal\":%.2f,"
    "\"thermalPeak\":%.2f,\"stall\":%s,\"packPercent\":%d,\"currentA\":%.2f,"
    "\"uptimeMs\":%lu}",
    l.distance, l.velocity, l.pwm, l.pitch, l.roll, l.battery, l.thermal,
    filteredDistance, velocity, (int)smoothedPWM, pitch, roll, batteryPercent,
    motorEnabled ? "true" : "false", (int)assistStrength, (int)sensitivity,
    leftOnline(now) ? "true" : "false", tofHealthy(now) ? "true" : "false",
    tofHasTarget(now) ? "true" : "false", thermalLoad,
    thermalPeak, stallFault ? "true" : "false", packPercent, motorCurrentA,
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
    // Re-enabling is the acknowledgement that clears a latched stall.
    if (motorEnabled) { stallFault = false; stallSinceMs = 0; }
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
    // Calibration aid: load and its peak are what issue #3 needs measured.
    Serial.printf("D %.0fmm | v %.0fmm/s (%.1f/sample) | tgt %d | pwm %d | "
                  "load %.2f peak %.2f%s | %s%s%s\n",
                  filteredDistance, velocityMmS, velocity, targetPWM, (int)smoothedPWM,
                  thermalLoad, thermalPeak,
                  CURRENT_SENSE_ENABLED ? "" : " (est)",
                  motorEnabled ? "on" : "off",
                  tofHealthy(now) ? "" : " | ToF FAULT",
                  stallFault ? " | STALL" : "");
  }

  esp_task_wdt_reset();
  delay(1);
}
