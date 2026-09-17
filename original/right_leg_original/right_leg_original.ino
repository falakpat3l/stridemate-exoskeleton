// =====================================================================
//  ORIGINAL prototype firmware (right leg) - kept for reference/history.
//  Written by B Dileep Kumar (github.com/Dileep195) for StrideMate.
//
//  This is the code exactly as handed over, except the Wi-Fi name and
//  password were removed. Use firmware/right_leg for the maintained version.
// =====================================================================

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <math.h>
#include "Adafruit_VL53L1X.h"

// =====================================================
// WIFI
// =====================================================

const char* ssid = "YOUR_WIFI_SSID";          // real value removed before publishing
const char* password = "YOUR_WIFI_PASSWORD";  // real value removed before publishing

IPAddress local_IP(10, 210, 60, 121);
IPAddress gateway(10, 210, 60, 1);
IPAddress subnet(255, 255, 255, 0);

WebServer server(80);

// =====================================================
// LEFT LEG ESP32 API
// =====================================================

String leftESP32 = "http://10.210.60.122/data";

// =====================================================
// MOTOR DRIVER
// =====================================================

#define RPWM 32
#define LPWM 33
#define REN 27
#define LEN 14
// =====================================================
// BATTERY MONITORING
// =====================================================
#define BATTERY_PIN 34
// =====================================================
// I2C BUSES
// =====================================================
TwoWire I2C_TOF = TwoWire(0);
TwoWire I2C_MPU = TwoWire(1);
// =====================================================
// TOF SENSOR
// =====================================================
Adafruit_VL53L1X vl53 = Adafruit_VL53L1X();
// =====================================================
// MPU6050
// =====================================================
const int MPU = 0x68;
int16_t AcX, AcY, AcZ;
int16_t GyX, GyY, GyZ;
float pitch = 0;
float roll = 0;
// =====================================================
// RIGHT LEG VARIABLES
// =====================================================
float filteredDistance = 0;
float previousDistance = 0;
float velocity = 0;

// =====================================================
// LEFT LEG VARIABLES
// =====================================================

float leftDistance = 0;
float leftVelocity = 0;
float leftPWM = 0;
float leftPitch = 0;
float leftRoll = 0;

// =====================================================
// MOTOR VARIABLES
// =====================================================

float smoothedPWM = 0;
int targetPWM = 0;

// =====================================================
// DASHBOARD VARIABLES
// =====================================================

int assistStrength = 255;
int sensitivity = 1;
bool motorEnabled = true;

// =====================================================
// FILTER SETTINGS
// =====================================================

const float DISTANCE_ALPHA = 0.7;
const float PWM_ALPHA = 0.35;

// =====================================================
// RANGE SETTINGS
// =====================================================

const int MIN_DISTANCE = 50;
const int MAX_DISTANCE = 450;

// =====================================================
// HTML PAGE
// =====================================================

String htmlPage() {

String html = R"rawliteral(
<!DOCTYPE html>
<html>

<head>

<meta name="viewport" content="width=device-width, initial-scale=1">

<script src="https://cdn.jsdelivr.net/npm/chart.js"></script>

<style>

body {
  font-family: Arial;
  background: #111;
  color: white;
  text-align: center;
  margin: 0;
}

.card {
  background: #222;
  margin: 15px;
  padding: 20px;
  border-radius: 15px;
}

.value {
  font-size: 30px;
  font-weight: bold;
}

.slider {
  width: 90%;
}

button {
  padding: 15px 30px;
  font-size: 18px;
  border-radius: 10px;
  border: none;
}

canvas {
  background: white;
  border-radius: 10px;
}

.row {
  display: flex;
  flex-wrap: wrap;
  justify-content: center;
}

.column {
  flex: 45%;
  margin: 10px;
}

</style>

</head>

<body>

<h1>ESP32 Exoskeleton Dashboard</h1>

<div class="row">

<div class="column">

<div class="card">
<h2>LEFT LEG</h2>

Distance:
<div class="value" id="leftDistance">0</div>

Velocity:
<div class="value" id="leftVelocity">0</div>

PWM:
<div class="value" id="leftPWM">0</div>

Pitch:
<div class="value" id="leftPitch">0</div>

Roll:
<div class="value" id="leftRoll">0</div>

</div>

</div>

<div class="column">

<div class="card">
<h2>RIGHT LEG</h2>

Distance:
<div class="value" id="rightDistance">0</div>

Velocity:
<div class="value" id="rightVelocity">0</div>

PWM:
<div class="value" id="rightPWM">0</div>

Pitch:
<div class="value" id="rightPitch">0</div>

Roll:
<div class="value" id="rightRoll">0</div>

</div>

</div>

</div>

<div class="card">
<h2>Battery</h2>
<div class="value" id="battery">0</div>
</div>

<div class="card">
<h2>Assist Strength</h2>
<input type="range" min="80" max="255" value="255" class="slider" id="assistSlider">
</div>

<div class="card">
<h2>Sensitivity</h2>
<input type="range" min="1" max="20" value="1" class="slider" id="sensitivitySlider">
</div>

<div class="card">
<button onclick="toggleMotor()">Toggle Motor</button>
</div>

<div class="card">
<canvas id="chart"></canvas>
</div>

<script>

const ctx = document.getElementById('chart').getContext('2d');

const chart = new Chart(ctx, {
type: 'line',

data: {
labels: [],
datasets: [

{
label: 'Left Distance',
data: [],
borderColor: 'red',
borderWidth: 2
},

{
label: 'Right Distance',
data: [],
borderColor: 'blue',
borderWidth: 2
}

]
},

options: {
responsive: true,
animation: false
}

});

async function fetchData() {

const response = await fetch('/data');

const data = await response.json();

document.getElementById('leftDistance').innerHTML = data.leftDistance;
document.getElementById('leftVelocity').innerHTML = data.leftVelocity;
document.getElementById('leftPWM').innerHTML = data.leftPWM;
document.getElementById('leftPitch').innerHTML = data.leftPitch;
document.getElementById('leftRoll').innerHTML = data.leftRoll;

document.getElementById('rightDistance').innerHTML = data.rightDistance;
document.getElementById('rightVelocity').innerHTML = data.rightVelocity;
document.getElementById('rightPWM').innerHTML = data.rightPWM;
document.getElementById('rightPitch').innerHTML = data.rightPitch;
document.getElementById('rightRoll').innerHTML = data.rightRoll;

document.getElementById('battery').innerHTML = data.battery + '%';

chart.data.labels.push('');

chart.data.datasets[0].data.push(data.leftDistance);
chart.data.datasets[1].data.push(data.rightDistance);

if(chart.data.labels.length > 40) {

chart.data.labels.shift();

chart.data.datasets[0].data.shift();
chart.data.datasets[1].data.shift();
}

chart.update();
}

setInterval(fetchData, 100);

assistSlider.oninput = function() {

fetch('/setPWM?value=' + this.value);
}

sensitivitySlider.oninput = function() {

fetch('/setSensitivity?value=' + this.value);
}

function toggleMotor() {

fetch('/toggleMotor');
}

</script>

</body>
</html>
)rawliteral";

return html;
}

// =====================================================
// BATTERY FUNCTION
// =====================================================

int readBatteryPercent() {

int raw = analogRead(BATTERY_PIN);

float voltage = (raw / 4095.0) * 3.3 * 2;

int percent = map(voltage * 100, 300, 420, 0, 100);

percent = constrain(percent, 0, 100);

return percent;
}

// =====================================================
// GET LEFT LEG DATA
// =====================================================

void getLeftLegData() {

HTTPClient http;

http.begin(leftESP32);

int httpCode = http.GET();

if(httpCode > 0) {

String payload = http.getString();

leftDistance =
payload.substring(
payload.indexOf(":") + 1,
payload.indexOf(",")
).toFloat();
}

http.end();
}

// =====================================================
// SETUP
// =====================================================

void setup() {

Serial.begin(115200);

// =====================================================
// MOTOR
// =====================================================

pinMode(RPWM, OUTPUT);
pinMode(LPWM, OUTPUT);

pinMode(REN, OUTPUT);
pinMode(LEN, OUTPUT);

digitalWrite(REN, HIGH);
digitalWrite(LEN, HIGH);

ledcAttach(RPWM, 2000, 8);
ledcAttach(LPWM, 2000, 8);

// =====================================================
// MPU6050
// =====================================================

I2C_MPU.begin(21, 22, 100000);

I2C_MPU.beginTransmission(MPU);
I2C_MPU.write(0x6B);
I2C_MPU.write(0);
I2C_MPU.endTransmission(true);

// =====================================================
// TOF
// =====================================================

I2C_TOF.begin(25, 26, 10000);

vl53.begin(0x29, &I2C_TOF);

vl53.startRanging();

// =====================================================
// WIFI
// =====================================================

WiFi.config(local_IP, gateway, subnet);

WiFi.begin(ssid, password);

while (WiFi.status() != WL_CONNECTED) {

delay(500);
}

Serial.println(WiFi.localIP());

// =====================================================
// OTA
// =====================================================

ArduinoOTA.begin();

// =====================================================
// WEB ROUTES
// =====================================================

server.on("/", []() {

server.send(200, "text/html", htmlPage());
});

server.on("/data", []() {

String json = "{";

json += "\"leftDistance\":" + String(leftDistance) + ",";
json += "\"leftVelocity\":" + String(leftVelocity) + ",";
json += "\"leftPWM\":" + String(leftPWM) + ",";
json += "\"leftPitch\":" + String(leftPitch) + ",";
json += "\"leftRoll\":" + String(leftRoll) + ",";

json += "\"rightDistance\":" + String(filteredDistance) + ",";
json += "\"rightVelocity\":" + String(velocity) + ",";
json += "\"rightPWM\":" + String((int)smoothedPWM) + ",";
json += "\"rightPitch\":" + String(pitch) + ",";
json += "\"rightRoll\":" + String(roll) + ",";

json += "\"battery\":" + String(readBatteryPercent());

json += "}";

server.send(200, "application/json", json);
});

server.on("/setPWM", []() {

assistStrength = server.arg("value").toInt();

server.send(200, "text/plain", "OK");
});

server.on("/setSensitivity", []() {

sensitivity = server.arg("value").toInt();

server.send(200, "text/plain", "OK");
});

server.on("/toggleMotor", []() {

motorEnabled = !motorEnabled;

server.send(200, "text/plain", "OK");
});

server.begin();
}

// =====================================================
// LOOP
// =====================================================

void loop() {

ArduinoOTA.handle();

server.handleClient();

getLeftLegData();

// =====================================================
// MPU READ
// =====================================================

I2C_MPU.beginTransmission(MPU);

I2C_MPU.write(0x3B);

I2C_MPU.endTransmission(false);

I2C_MPU.requestFrom(MPU, 14, true);

if (I2C_MPU.available() >= 14) {

AcX = I2C_MPU.read() << 8 | I2C_MPU.read();
AcY = I2C_MPU.read() << 8 | I2C_MPU.read();
AcZ = I2C_MPU.read() << 8 | I2C_MPU.read();

I2C_MPU.read();
I2C_MPU.read();

GyX = I2C_MPU.read() << 8 | I2C_MPU.read();
GyY = I2C_MPU.read() << 8 | I2C_MPU.read();
GyZ = I2C_MPU.read() << 8 | I2C_MPU.read();
}

pitch = atan2(AcY, AcZ) * 180 / PI;

roll = atan2(AcX, AcZ) * 180 / PI;

// =====================================================
// TOF READ
// =====================================================

int rawDistance = vl53.distance();

if(rawDistance > 0 && rawDistance < 2000) {

filteredDistance =
(DISTANCE_ALPHA * rawDistance) +
((1 - DISTANCE_ALPHA) * filteredDistance);
}

velocity = filteredDistance - previousDistance;

previousDistance = filteredDistance;

// =====================================================
// PWM CONTROL
// =====================================================

bool inRange = false;

if(filteredDistance >= MIN_DISTANCE &&
filteredDistance <= MAX_DISTANCE) {

inRange = true;
}

if(motorEnabled &&
inRange &&
abs(velocity) > sensitivity) {

float v = abs(velocity);

targetPWM = 80 + (pow(v, 1.5) * 2.5);

targetPWM = constrain(targetPWM, 80, assistStrength);
}

else {

targetPWM = 0;
}

smoothedPWM =
(PWM_ALPHA * targetPWM) +
((1 - PWM_ALPHA) * smoothedPWM);

smoothedPWM = constrain(smoothedPWM, 0, 255);

// =====================================================
// MOTOR OUTPUT
// =====================================================

if(velocity > sensitivity) {

ledcWrite(RPWM, (int)smoothedPWM);
ledcWrite(LPWM, 0);
}

else if(velocity < -sensitivity) {

ledcWrite(RPWM, 0);
ledcWrite(LPWM, (int)smoothedPWM);
}

else {

ledcWrite(RPWM, 0);
ledcWrite(LPWM, 0);
}
// =====================================================
// SERIAL OUTPUT
// =====================================================

Serial.print("Distance: ");
Serial.print(filteredDistance);

Serial.print(" mm | Velocity: ");
Serial.print(velocity);

Serial.print(" | TargetPWM: ");
Serial.print(targetPWM);

Serial.print(" | SmoothPWM: ");
Serial.println((int)smoothedPWM);

delay(10);
}
