// =====================================================================
//  StrideMate - web dashboard served by the RIGHT LEG ESP32
//
//  Stored in flash (PROGMEM), no internet needed: the live chart is drawn
//  with a tiny built-in canvas plotter instead of loading Chart.js from a CDN.
// =====================================================================
#pragma once
#include <Arduino.h>

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>StrideMate Dashboard</title>
<style>
  body { font-family: Arial, sans-serif; background:#111; color:#fff; text-align:center; margin:0; }
  h1 { margin:16px 8px 4px; }
  .status { font-size:14px; color:#aaa; margin-bottom:8px; }
  .card { background:#222; margin:12px; padding:16px; border-radius:15px; }
  .row { display:flex; flex-wrap:wrap; justify-content:center; }
  .column { flex:1 1 280px; }
  .grid { display:grid; grid-template-columns:auto auto; gap:6px 12px; text-align:left; max-width:260px; margin:0 auto; }
  .value { font-size:26px; font-weight:bold; text-align:right; }
  .slider { width:90%; }
  button { padding:15px 30px; font-size:18px; border-radius:10px; border:none; margin:6px; cursor:pointer; }
  .on  { background:#2e7d32; color:#fff; }
  .off { background:#555; color:#fff; }
  .stop { background:#c62828; color:#fff; font-weight:bold; }
  .badge { display:inline-block; padding:3px 10px; border-radius:10px; font-size:13px; }
  .ok { background:#2e7d32; } .bad { background:#c62828; } .warn { background:#ef6c00; }
  .info { background:#37474f; }
  .note { font-size:11.5px; color:#999; margin-top:8px; }
  .hidden { display:none; }
  .stallbar {
    background:#c62828; color:#fff; font-weight:bold; border-radius:12px;
    margin:12px; padding:14px; font-size:16px;
  }
  canvas { background:#fff; border-radius:10px; width:100%; height:220px; }
  .legend span { margin:0 10px; font-size:14px; }
</style>
</head>
<body>
<h1>StrideMate Exoskeleton</h1>
<div class="status" id="conn">connecting...</div>

<div class="stallbar hidden" id="stallBar">STALL</div>

<div class="card">
  <div>Motor: <b id="motorState">?</b></div>
  <button id="motorBtn" class="off" onclick="setMotor(!motorOn)">Enable motor</button>
  <button class="stop" onclick="stopAll()">STOP</button>
</div>

<div class="row">
  <div class="column"><div class="card">
    <h2>LEFT LEG <span class="badge bad" id="leftBadge">offline</span></h2>
    <div class="grid">
      <span>Distance (mm)</span><span class="value" id="leftDistance">0</span>
      <span>Velocity</span><span class="value" id="leftVelocity">0</span>
      <span>PWM</span><span class="value" id="leftPWM">0</span>
      <span>Pitch</span><span class="value" id="leftPitch">0</span>
      <span>Roll</span><span class="value" id="leftRoll">0</span>
    </div>
  </div></div>
  <div class="column"><div class="card">
    <h2>RIGHT LEG <span class="badge ok" id="tofBadge">sensor ok</span></h2>
    <div class="grid">
      <span>Distance (mm)</span><span class="value" id="rightDistance">0</span>
      <span>Velocity</span><span class="value" id="rightVelocity">0</span>
      <span>PWM</span><span class="value" id="rightPWM">0</span>
      <span>Pitch</span><span class="value" id="rightPitch">0</span>
      <span>Roll</span><span class="value" id="rightRoll">0</span>
    </div>
  </div></div>
</div>

<div class="card">
  <h2>Battery</h2>
  <div class="grid">
    <span>Right logic</span><span class="value" id="battery">0%</span>
    <span>Left logic</span><span class="value" id="leftBattery">-</span>
    <span>Right pack</span><span class="value" id="pack">-</span>
    <span>Left pack</span><span class="value" id="leftPack">-</span>
  </div>
  <div class="note" id="packNote">Motor pack not monitored. The logic figures say nothing about it.</div>
</div>

<div class="card">
  <h2>Motor load <span class="badge ok" id="thermBadge">normal</span></h2>
  <div class="grid">
    <span>Right leg</span><span class="value" id="thermal">0%</span>
    <span>Left leg</span><span class="value" id="leftThermal">-</span>
    <span>Right peak</span><span class="value" id="thermalPeak">0%</span>
    <span>Left peak</span><span class="value" id="leftThermalPeak">-</span>
    <span>Right current</span><span class="value" id="current">-</span>
    <span>Left current</span><span class="value" id="leftCurrent">-</span>
  </div>
  <div class="note" id="loadNote">Estimated from duty cycle. Peak is the number to calibrate against.</div>
</div>

<div class="card">
  <h2>Assist Strength: <span id="assistVal">255</span></h2>
  <input type="range" min="80" max="255" value="255" class="slider" id="assistSlider">
</div>

<div class="card">
  <h2>Sensitivity: <span id="sensVal">1</span></h2>
  <input type="range" min="1" max="20" value="1" class="slider" id="sensitivitySlider">
</div>

<div class="card">
  <div class="legend"><span style="color:#e53935">&#9632; Left distance</span><span style="color:#1e88e5">&#9632; Right distance</span></div>
  <canvas id="chart" width="600" height="220"></canvas>
</div>

<script>
const $ = (id) => document.getElementById(id);
const HISTORY = 40;
const leftHist = [], rightHist = [];
let motorOn = false;
let slidersInitialised = false;

function drawChart() {
  const c = $('chart'), g = c.getContext('2d');
  const w = c.width, h = c.height, pad = 30;
  g.clearRect(0, 0, w, h);
  const all = leftHist.concat(rightHist);
  const max = Math.max(100, ...all) * 1.1;
  g.strokeStyle = '#ddd'; g.fillStyle = '#666'; g.font = '11px Arial'; g.lineWidth = 1;
  for (let i = 0; i <= 4; i++) {
    const y = pad / 2 + (h - pad) * i / 4;
    g.beginPath(); g.moveTo(pad, y); g.lineTo(w, y); g.stroke();
    g.fillText(Math.round(max * (1 - i / 4)), 2, y + 4);
  }
  const plot = (arr, color) => {
    if (arr.length < 2) return;
    g.strokeStyle = color; g.lineWidth = 2; g.beginPath();
    arr.forEach((v, i) => {
      const x = pad + (w - pad) * i / (HISTORY - 1);
      const y = pad / 2 + (h - pad) * (1 - v / max);
      i ? g.lineTo(x, y) : g.moveTo(x, y);
    });
    g.stroke();
  };
  plot(leftHist, '#e53935');
  plot(rightHist, '#1e88e5');
}

function push(arr, v) { arr.push(v); if (arr.length > HISTORY) arr.shift(); }

function setMotorUi(on) {
  motorOn = on;
  $('motorState').textContent = on ? 'ENABLED' : 'disabled';
  $('motorBtn').textContent = on ? 'Disable motor' : 'Enable motor';
  $('motorBtn').className = on ? 'on' : 'off';
}

async function poll() {
  try {
    const r = await fetch('/data', { cache: 'no-store' });
    const d = await r.json();
    ['leftDistance','leftVelocity','leftPWM','leftPitch','leftRoll',
     'rightDistance','rightVelocity','rightPWM','rightPitch','rightRoll']
      .forEach(k => $(k).textContent = d[k]);
    $('battery').textContent = d.battery + '%';
    $('leftBattery').textContent = d.leftOnline ? d.leftBattery + '%' : '-';
    $('leftBadge').textContent = d.leftOnline ? 'online' : 'offline';
    $('leftBadge').className = 'badge ' + (d.leftOnline ? 'ok' : 'bad');
    $('tofBadge').textContent = !d.tofOk ? 'SENSOR FAULT' : (d.tofTarget ? 'sensor ok' : 'no target');
    $('tofBadge').className = 'badge ' + (!d.tofOk ? 'bad' : (d.tofTarget ? 'ok' : 'warn'));
    const rt = Math.round(d.thermal * 100);
    $('thermal').textContent = rt + '%';
    $('leftThermal').textContent = d.leftOnline ? Math.round(d.leftThermal * 100) + '%' : '-';
    $('thermalPeak').textContent = Math.round((d.thermalPeak || 0) * 100) + '%';
    $('leftThermalPeak').textContent = d.leftOnline
      ? Math.round((d.leftThermalPeak || 0) * 100) + '%' : '-';

    // packPercent is -1 when no divider is wired. That is not 0 %.
    const pk = (v) => (v == null || v < 0) ? 'not wired' : Math.round(v) + '%';
    $('pack').textContent = pk(d.packPercent);
    $('leftPack').textContent = d.leftOnline ? pk(d.leftPackPercent) : '-';
    $('packNote').style.display =
      (d.packPercent >= 0 || (d.leftOnline && d.leftPackPercent >= 0)) ? 'none' : '';

    const amps = (v) => (v == null || v <= 0) ? 'not wired' : (+v).toFixed(1) + ' A';
    $('current').textContent = amps(d.currentA);
    $('leftCurrent').textContent = d.leftOnline ? amps(d.leftCurrentA) : '-';
    $('loadNote').textContent = (d.currentA > 0 || (d.leftOnline && d.leftCurrentA > 0))
      ? 'Measured from motor current.'
      : 'Estimated from duty cycle. Peak is the number to calibrate against.';

    // SAFETY: a latched stall has stopped the motor. Say so unmissably, and
    // say how to clear it - re-enabling is the acknowledgement.
    const rs = !!d.stall, ls = d.leftOnline && !!d.leftStall;
    const cmdErr = Date.now() < cmdErrorUntil;   // keep a failed-command warning up
    if (!cmdErr) $('stallBar').classList.toggle('hidden', !(rs || ls));
    if ((rs || ls) && !cmdErr) {
      $('stallBar').textContent = 'STALL - ' +
        (rs && ls ? 'both legs' : rs ? 'right leg' : 'left leg') +
        ' latched off. Clear the obstruction, then tap Enable motor.';
    }
    const hot = d.thermal >= 0.8 || (d.leftOnline && d.leftThermal >= 0.8);
    $('thermBadge').textContent = hot ? 'ASSIST REDUCED' : 'normal';
    $('thermBadge').className = 'badge ' + (hot ? 'warn' : 'ok');
    setMotorUi(d.motorEnabled);
    if (!slidersInitialised) {
      $('assistSlider').value = d.assistStrength; $('assistVal').textContent = d.assistStrength;
      $('sensitivitySlider').value = d.sensitivity; $('sensVal').textContent = d.sensitivity;
      slidersInitialised = true;
    }
    push(leftHist, d.leftDistance); push(rightHist, d.rightDistance); drawChart();
    $('conn').textContent = 'live - uptime ' + Math.round(d.uptimeMs / 1000) + ' s';
  } catch (e) {
    $('conn').textContent = 'connection lost - retrying...';
  }
  setTimeout(poll, 100);   // next request only after this one finished
}

// Send slider changes at most every 150 ms so the ESP32 is not flooded.
function throttled(url) {
  let timer = null, latest = null;
  return (value) => {
    latest = value;
    if (timer) return;
    timer = setTimeout(() => { timer = null; fetch(url + latest, { method: 'POST' }); }, 150);
  };
}
const sendAssist = throttled('/setPWM?value=');
const sendSens = throttled('/setSensitivity?value=');
$('assistSlider').addEventListener('input', function () {
  $('assistVal').textContent = this.value; sendAssist(this.value);
});
$('sensitivitySlider').addEventListener('input', function () {
  $('sensVal').textContent = this.value; sendSens(this.value);
});
// SAFETY: fetch() only throws on a network failure; a 401/403/500 still
// "succeeds". Only show the new state once the board has confirmed it, and
// make a failed command impossible to miss - above all a failed STOP.
let cmdErrorUntil = 0;
function cmdFailed(what, why) {
  cmdErrorUntil = Date.now() + 8000;
  $('stallBar').textContent = what + ' FAILED (' + why + '). The motor state has NOT changed.';
  $('stallBar').classList.remove('hidden');
}
async function stopAll() {
  try {
    const r = await fetch('/stop', { method: 'POST' });
    if (!r.ok) return cmdFailed('STOP', 'HTTP ' + r.status);
    setMotorUi(false);
  } catch (e) { cmdFailed('STOP', 'no connection'); }
}
async function setMotor(on) {
  const what = on ? 'Enable motor' : 'Disable motor';
  try {
    const r = await fetch('/motor?on=' + (on ? 1 : 0), { method: 'POST' });
    if (!r.ok) return cmdFailed(what, 'HTTP ' + r.status);
    setMotorUi(on);
  } catch (e) { cmdFailed(what, 'no connection'); }
}

drawChart();
poll();
</script>
</body>
</html>
)rawliteral";
