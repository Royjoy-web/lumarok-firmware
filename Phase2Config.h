#pragma once
// Phase2Config.h — additive Config additions for Phase 2
// Drop into firmware alongside Phase 1 Config.h; include AFTER Config.h in main .ino
// All defines use #ifndef so they never override existing values.

// ── Phase 2 Feature Flags ─────────────────────────────────────
#ifndef ENABLE_RGBW
  #define ENABLE_RGBW         false
#endif
#ifndef ENABLE_HVAC
  #define ENABLE_HVAC         false
#endif
#ifndef ENABLE_SOIL
  #define ENABLE_SOIL         false
#endif
#ifndef ENABLE_NFC
  #define ENABLE_NFC          false
#endif
#ifndef ENABLE_MULTIMODAL
  #define ENABLE_MULTIMODAL   false  // F14/F15
#endif
#ifndef ENABLE_CSI_PRESENCE
  #define ENABLE_CSI_PRESENCE false  // F23
#endif
#ifndef ENABLE_OTA_DELTA
  #define ENABLE_OTA_DELTA    false  // F27
#endif
#ifndef ENABLE_WIFI_ROAMING
  #define ENABLE_WIFI_ROAMING false  // F30
#endif
#ifndef ENABLE_DEEP_IDLE
  #define ENABLE_DEEP_IDLE    false  // F29
#endif

// ── Flags referenced by CommandDispatcher.h for always-compiled drivers ───
// (FingerprintDriver and IRTransmitter classes are unconditionally compiled
// in drivers/; these flags only gate their MQTT command handlers, so they
// default to true rather than false.)
#ifndef ENABLE_FINGERPRINT
  #define ENABLE_FINGERPRINT  true
#endif
#ifndef ENABLE_IR_TX
  #define ENABLE_IR_TX        true
#endif

// ── Flags referenced by tasks/SensorTaskV2_P2.h with NO driver class yet ──
// LD2410Driver, SCD41Driver, AirQualityDriver, WaterLeakDriver, SmokeDriver,
// VibrationDriver, WiegandDriver, and SunriseSunset are referenced in
// SensorTaskV2_P2.h but no class definition exists anywhere in this firmware
// yet. These flags previously had no #define at all, so they silently
// evaluated to 0/false and the dead code never compiled — flipping any of
// them on in build_flags would currently fail with "class not declared",
// which is a confusing error far from its actual cause. Declared explicitly
// here, defaulted off, with a loud #error if enabled before the driver
// exists, so the failure points at the real problem.
#ifndef ENABLE_MMWAVE
  #define ENABLE_MMWAVE       false  // needs LD2410Driver — not yet implemented
#endif
#ifndef ENABLE_SCD41
  #define ENABLE_SCD41        false  // needs SCD41Driver — not yet implemented
#endif
#ifndef ENABLE_AIR_QUALITY
  #define ENABLE_AIR_QUALITY  false  // needs AirQualityDriver — not yet implemented
#endif
#ifndef ENABLE_WATER_LEAK
  #define ENABLE_WATER_LEAK   false  // needs WaterLeakDriver — not yet implemented
#endif
#ifndef ENABLE_SMOKE
  #define ENABLE_SMOKE        false  // needs SmokeDriver — not yet implemented
#endif
#ifndef ENABLE_VIBRATION
  #define ENABLE_VIBRATION    false  // needs VibrationDriver — not yet implemented
#endif
#ifndef ENABLE_WIEGAND
  #define ENABLE_WIEGAND      false  // needs WiegandDriver — not yet implemented
#endif
#ifndef ENABLE_SUNRISE
  #define ENABLE_SUNRISE      false  // needs SunriseSunset — not yet implemented
#endif

#if ENABLE_MMWAVE
  #error "ENABLE_MMWAVE=true: LD2410Driver has no implementation in this firmware yet. Add drivers/LD2410Driver.h or leave ENABLE_MMWAVE=false."
#endif
#if ENABLE_SCD41
  #error "ENABLE_SCD41=true: SCD41Driver has no implementation in this firmware yet. Add the driver or leave ENABLE_SCD41=false."
#endif
#if ENABLE_AIR_QUALITY
  #error "ENABLE_AIR_QUALITY=true: AirQualityDriver has no implementation in this firmware yet. Add the driver or leave ENABLE_AIR_QUALITY=false."
#endif
#if ENABLE_WATER_LEAK
  #error "ENABLE_WATER_LEAK=true: WaterLeakDriver has no implementation in this firmware yet. Add the driver or leave ENABLE_WATER_LEAK=false."
#endif
#if ENABLE_SMOKE
  #error "ENABLE_SMOKE=true: SmokeDriver has no implementation in this firmware yet. Add the driver or leave ENABLE_SMOKE=false."
#endif
#if ENABLE_VIBRATION
  #error "ENABLE_VIBRATION=true: VibrationDriver has no implementation in this firmware yet. Add the driver or leave ENABLE_VIBRATION=false."
#endif
#if ENABLE_WIEGAND
  #error "ENABLE_WIEGAND=true: WiegandDriver has no implementation in this firmware yet. Add the driver or leave ENABLE_WIEGAND=false."
#endif
#if ENABLE_SUNRISE
  #error "ENABLE_SUNRISE=true: SunriseSunset has no implementation in this firmware yet. Add the driver or leave ENABLE_SUNRISE=false."
#endif

// ── Phase 2 GPIO ───────────────────────────────────────────────
// Free-GPIO audit performed against the FULL pin set in core/Config.h,
// including the Phase-1 ultrasonic/door/window/RGB fixes. See the comment
// block above PIN_US2_TRIG in Config.h for the methodology.
#if defined(ARDUINO_ESP32S3_DEV)
  // S3: GPIO38,42,45,46,47 confirmed free (not flash/PSRAM/USB/console, not
  // used by any relay/sensor/ultrasonic/RGB pin defined in Config.h).
  #define PIN_RGBW_R          GPIO_NUM_38
  #define PIN_RGBW_G          GPIO_NUM_42  // was GPIO39 — conflicted with PIN_DHT_SENSOR
  #define PIN_RGBW_B          GPIO_NUM_45  // was GPIO40 — conflicted with PIN_DOOR_SENSOR
  #define PIN_RGBW_W          GPIO_NUM_46  // was GPIO41 — conflicted with PIN_WINDOW_SENSOR
  #define PIN_SOIL             GPIO_NUM_47  // was GPIO4 — conflicted with PIN_DOOR_LOCK
  #define SOIL_WATER_THRESHOLD_PCT 35       // water if < 35% moisture
  #define RGBW_AVAILABLE       1
  #define SOIL_GPIO_AVAILABLE  1
  // CSI config
  #define CSI_MOTION_THRESHOLD 50.0f
  #define CSI_HISTORY_LEN      16
  #define CSI_MOTION_WINDOW_MS 5000

  static_assert(PIN_RGBW_G != PIN_DHT_SENSOR,    "PIN_RGBW_G conflicts with PIN_DHT_SENSOR.");
  static_assert(PIN_RGBW_B != PIN_DOOR_SENSOR,   "PIN_RGBW_B conflicts with PIN_DOOR_SENSOR.");
  static_assert(PIN_RGBW_W != PIN_WINDOW_SENSOR, "PIN_RGBW_W conflicts with PIN_WINDOW_SENSOR.");
  static_assert(PIN_SOIL   != PIN_DOOR_LOCK,     "PIN_SOIL conflicts with PIN_DOOR_LOCK.");
#else
  // Classic ESP32: per the free-GPIO audit in Config.h, this board has ZERO
  // free GPIOs once the base device set is assigned — RGBW and soil-moisture
  // via direct GPIO are not physically possible here. Force both off rather
  // than silently sharing a pin with another device.
  #if ENABLE_RGBW
    #error "ENABLE_RGBW=true on classic ESP32: no free GPIO remains on this board " \
"for a 4-channel RGBW driver. Use an S3 board, drive RGBW via an I2C PWM " \
"expander (e.g. PCA9685) instead of direct GPIO, or leave ENABLE_RGBW=false."
  #endif
  #if ENABLE_SOIL
    #error "ENABLE_SOIL=true on classic ESP32: no free GPIO remains on this board " \
"for a soil-moisture analog input. Use an S3 board, read soil moisture via " \
"an I2C/ADC expander instead of direct GPIO, or leave ENABLE_SOIL=false."
  #endif
  #define RGBW_AVAILABLE       0
  #define SOIL_GPIO_AVAILABLE  0
  #define CSI_MOTION_THRESHOLD 50.0f
  #define CSI_HISTORY_LEN      16
  #define CSI_MOTION_WINDOW_MS 5000
#endif

// ── NVSSceneCache stub (full implementation in future Phase 2 driver release) ──
#ifndef NVS_SCENE_CACHE_DEFINED
#define NVS_SCENE_CACHE_DEFINED
struct NVSSceneCache {
    static void init() {}   // stub — full scene caching TBD
};
#endif
