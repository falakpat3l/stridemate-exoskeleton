// =====================================================================
//  StrideMate - RIGHT LEG configuration
//
//  Everything you might want to tune lives here. Values marked
//  "(original)" are exactly what the first working prototype used.
// =====================================================================
#pragma once

// ---------------------------------------------------------------------
//  Network (static IPs, must match your hotspot/router subnet)
// ---------------------------------------------------------------------
#define RIGHT_LEG_IP        10, 210, 60, 121   // this board (original)
#define NETWORK_GATEWAY     10, 210, 60, 1     // (original)
#define NETWORK_SUBNET      255, 255, 255, 0   // (original)
#define LEFT_LEG_HOST       "10.210.60.122"    // left-leg ESP32 (original)
#define OTA_HOSTNAME        "stridemate-right"

// ---------------------------------------------------------------------
//  Pins
// ---------------------------------------------------------------------
// Motor driver (BTS7960-style dual half-bridge: RPWM/LPWM + enables)
#define PIN_MOTOR_RPWM      32   // (original)
#define PIN_MOTOR_LPWM      33   // (original)
#define PIN_MOTOR_REN       27   // (original)
#define PIN_MOTOR_LEN       14   // (original)

#define PIN_BATTERY_ADC     34   // (original) battery voltage via divider

// I2C bus 0 -> VL53L1X time-of-flight distance sensor
#define PIN_TOF_SDA         25   // (original)
#define PIN_TOF_SCL         26   // (original)
#define TOF_I2C_FREQ_HZ     10000   // (original) slow bus, robust on long wires

// I2C bus 1 -> MPU6050 accelerometer / gyroscope
#define PIN_MPU_SDA         21   // (original)
#define PIN_MPU_SCL         22   // (original)
#define MPU_I2C_FREQ_HZ     100000  // (original)
#define MPU_I2C_ADDR        0x68    // (original)

// ---------------------------------------------------------------------
//  Motor PWM
// ---------------------------------------------------------------------
#define MOTOR_PWM_FREQ_HZ   2000 // (original)
#define MOTOR_PWM_BITS      8    // (original) -> duty range 0..255

// ---------------------------------------------------------------------
//  Assist control
// ---------------------------------------------------------------------
#define MIN_DISTANCE_MM     50     // (original) assist only inside this window
#define MAX_DISTANCE_MM     450    // (original)
#define TOF_VALID_MAX_MM    2000   // (original) readings above this are ignored

#define DISTANCE_ALPHA      0.7f   // (original) distance low-pass filter
#define PWM_ALPHA           0.35f  // (original) PWM ramp smoothing

#define PWM_MIN_ASSIST      80     // (original) lowest non-zero motor duty
#define PWM_CURVE_GAIN      2.5f   // (original) duty = 80 + gain * |v|^exp
#define PWM_CURVE_EXPONENT  1.5f   // (original)

#define DEFAULT_ASSIST_STRENGTH 255 // (original) max duty, dashboard slider 80..255
#define DEFAULT_SENSITIVITY     1   // (original) velocity dead-band, slider 1..20

// SAFETY: motor starts DISABLED after power-on. Press "Enable motor" on the
// dashboard to start assisting. (The original prototype started enabled.)
#define MOTOR_ENABLED_AT_BOOT   false

// Fixed control-loop period. Smoothing and motor output update at this rate.
#define CONTROL_PERIOD_MS       10     // (original loop had delay(10))

// SAFETY: if no fresh distance reading arrives within this time, the motor
// is switched off until readings return.
#define TOF_STALE_TIMEOUT_MS    500

// SAFETY: a lost-then-re-acquired target must not look like a huge movement.
// If no VALID reading arrived for longer than this, the distance filter is
// re-seeded from the new reading instead of differenced against a stale one.
#define TOF_RESEED_AFTER_MS     150

// SAFETY: the sensor can be alive and ranging while seeing no target at all.
// If nothing valid arrives for this long, the motor is switched off.
#define TOF_NO_TARGET_TIMEOUT_MS 500

// SAFETY: hard ceiling on one sample's velocity (mm per sample). Anything
// larger is a sensor artefact, not a leg movement.
#define MAX_VELOCITY_MM         30.0f

// When the motor must reverse direction, ramp from zero instead of jumping
// straight to full reverse power (protects gearbox, driver and the user).
#define RESET_RAMP_ON_REVERSAL  true

// VL53L1X timing budget in ms (15, 20, 33, 50, 100, 200, 500).
// This is a CONTROL GAIN, not just a sensor setting: velocity is measured in
// mm per sample, so the sample period scales it directly. Leaving it at 0
// ("library default") meant a library update could silently re-tune the
// exoskeleton. 33 ms balances noise against latency; if you change it,
// re-check Sensitivity on the bench.
#define TOF_TIMING_BUDGET_MS    33
// Direction of positive velocity for THIS leg. Set to -1 if the motor or the
// linkage is mirrored and the leg is driven the wrong way. Verify with the
// motor mechanically disconnected before trusting it.
#define MOTOR_DIRECTION_SIGN    1


// ---------------------------------------------------------------------
//  Left-leg link
// ---------------------------------------------------------------------
#define LEFT_POLL_PERIOD_MS     100    // how often to fetch left-leg data
#define LEFT_HTTP_TIMEOUT_MS    150    // give up quickly if left leg is silent
#define LEFT_OFFLINE_AFTER_MS   1000   // shown as "offline" on the dashboard

// If true, the right motor also stops while the left leg is offline, so the
// user is never assisted on one side only. Original behaviour = false.
#define STOP_MOTOR_IF_LEFT_OFFLINE false

// ---------------------------------------------------------------------
//  Battery (1-cell Li-ion through a 2:1 divider in the original build)
// ---------------------------------------------------------------------
#define BATTERY_DIVIDER_RATIO   2.0f   // (original)
#define BATTERY_EMPTY_V         3.00f  // (original) 0 %
#define BATTERY_FULL_V          4.20f  // (original) 100 %

// ---------------------------------------------------------------------
//  Motor thermal budget  (OPEN LOOP - read docs/safety.md before trusting it)
// ---------------------------------------------------------------------
// There is no current sensor and no thermistor on this build, so heating is
// ESTIMATED from the duty cycle: thermalLoad integrates duty^2 and decays
// with THERMAL_COOL_TAU_S. Past THERMAL_WARN_LOAD the assist ceiling is
// folded back smoothly down to THERMAL_MIN_ASSIST_FRAC of its range. Folding
// back rather than cutting out matters: losing assist abruptly mid-stride is
// itself a hazard, and a smaller duty also reduces heating, so the loop
// settles instead of oscillating.
//
// OFF BY DEFAULT. The constants below are PLACEHOLDERS, tuned in simulation so
// that no realistic walking profile folds back and a motor held at full duty
// folds back after about a minute. Nobody has measured the real motor, and an
// untuned fold-back surprising you mid-walk is its own hazard - so the feature
// ships dormant.
//
// The load estimate still runs and is still shown on the dashboard, which is
// exactly what you need to calibrate: walk the device, watch what load your
// real use produces, then set THERMAL_FULL_DUTY_S and THERMAL_COOL_TAU_S so
// that normal use stays well under 0.80 and abuse does not. Turn this on only
// once those numbers come from the bench rather than from simulation. Better
// still, wire the BTS7960's R_IS/L_IS current-sense outputs to a spare ADC and
// close the loop on real current instead of estimating from duty.
#define THERMAL_PROTECTION       false
#define THERMAL_FULL_DUTY_S      40.0f  // duty^2-seconds to reach load 1.0
#define THERMAL_COOL_TAU_S       45.0f  // exponential cooling time constant
#define THERMAL_WARN_LOAD        0.80f  // fold-back starts here
#define THERMAL_MIN_ASSIST_FRAC  0.25f  // never fold below this share of range

// ---------------------------------------------------------------------
//  Fault handling
// ---------------------------------------------------------------------
// SAFETY: an I2C transaction that never completes stops the control loop,
// while the LEDC peripheral keeps applying the last duty cycle in hardware.
// Bound both buses so a wedged sensor cannot hold the motor on.
#define MPU_I2C_TIMEOUT_MS      20
#define TOF_I2C_TIMEOUT_MS      20

// SAFETY: if loop() stops feeding the watchdog for this long the board
// resets, which lands in the motor-disabled-at-boot state.
#define CONTROL_WDT_TIMEOUT_MS  1000

// ---------------------------------------------------------------------
//  Serial debug output
// ---------------------------------------------------------------------
#define SERIAL_BAUD             115200 // (original)
#define SERIAL_PRINT_PERIOD_MS  100    // original printed every loop (~10 ms)
