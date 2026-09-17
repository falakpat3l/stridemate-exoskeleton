// =====================================================================
//  StrideMate - LEFT LEG configuration  (RECONSTRUCTED - see left_leg.ino)
//
//  Everything you might want to tune lives here. Values marked
//  "(original)" are exactly what the first working prototype used.
// =====================================================================
#pragma once

// ---------------------------------------------------------------------
//  Network (static IPs, must match your hotspot/router subnet)
// ---------------------------------------------------------------------
#define LEFT_LEG_IP         10, 210, 60, 122   // this board (address the original right leg polled)
#define RIGHT_LEG_IP        10, 210, 60, 121   // right-leg ESP32 (original)
#define NETWORK_GATEWAY     10, 210, 60, 1     // (original)
#define NETWORK_SUBNET      255, 255, 255, 0   // (original)
#define OTA_HOSTNAME        "stridemate-left"

// ---------------------------------------------------------------------
//  Pins  - ASSUMED identical wiring to the right leg. Verify on the hardware!
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

// SAFETY: motor starts DISABLED. It is enabled only when the right leg
// forwards "motor on" from the dashboard.
#define MOTOR_ENABLED_AT_BOOT   false

// Fixed control-loop period. Smoothing and motor output update at this rate.
#define CONTROL_PERIOD_MS       10     // (original loop had delay(10))

// SAFETY: if no fresh distance reading arrives within this time, the motor
// is switched off until readings return.
#define TOF_STALE_TIMEOUT_MS    500

// When the motor must reverse direction, ramp from zero instead of jumping
// straight to full reverse power (protects gearbox, driver and the user).
#define RESET_RAMP_ON_REVERSAL  true

// Optional: VL53L1X timing budget in ms (15, 20, 33, 50, 100, 200, 500).
// 0 = keep the library default, which is what the original prototype used.
#define TOF_TIMING_BUDGET_MS    0

// ---------------------------------------------------------------------
//  Link to the right leg
// ---------------------------------------------------------------------
// SAFETY: the right leg polls this board ~10x per second. If nothing is heard
// from it for this long (right leg off, crashed or out of Wi-Fi range), the
// left motor stops, because the dashboard can no longer switch it off.
#define STOP_MOTOR_IF_RIGHT_SILENT  true
#define RIGHT_SILENT_TIMEOUT_MS     1000

// ---------------------------------------------------------------------
//  Battery (1-cell Li-ion through a 2:1 divider in the original build)
// ---------------------------------------------------------------------
#define BATTERY_DIVIDER_RATIO   2.0f   // (original)
#define BATTERY_EMPTY_V         3.00f  // (original) 0 %
#define BATTERY_FULL_V          4.20f  // (original) 100 %

// ---------------------------------------------------------------------
//  Serial debug output
// ---------------------------------------------------------------------
#define SERIAL_BAUD             115200 // (original)
#define SERIAL_PRINT_PERIOD_MS  100    // original printed every loop (~10 ms)
