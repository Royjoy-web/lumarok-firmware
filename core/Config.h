#pragma once
#include <Arduino.h>
#include <esp_idf_version.h>
#include <esp_task_wdt.h>
#include "Types.h"

// ── Build Identity ────────────────────────────────────────────
#define FIRMWARE_VERSION     "3.2.0"
#define HARDWARE_REVISION    "C10"

// ── Deployment Mode ───────────────────────────────────────────
#define MASTER_ONLY          1
#define MASTER_WITH_NODES    2
#ifndef DEPLOYMENT_MODE
  #define DEPLOYMENT_MODE    MASTER_ONLY
#endif

// ── Feature Flags (override via build_flags in platformio.ini) ─
#ifndef ENABLE_BLE
  #define ENABLE_BLE         true
#endif
#ifndef ENABLE_SERIAL_PROV      // Disable in production OTA images
  #define ENABLE_SERIAL_PROV  false
#endif
#ifndef OFFLINE_MODE
  #define OFFLINE_MODE       false
#endif
#ifndef USE_RGB
  #define USE_RGB            false
#endif
#ifndef USE_I2C_EXPANDER
  // ArduinoDroid has no build_flags mechanism (unlike PlatformIO's
  // wrover_production env) to set this externally, so it needs a real
  // default here. On classic/PSRAM boards (e.g. WROVER-E) there is no GPIO
  // alternative for door/window sensors — default to expecting a PCF8574.
  // On S3, direct GPIO works, so default off. Override by #define-ing this
  // above your #include "core/Config.h" line if your wiring differs.
  #if defined(ARDUINO_ESP32S3_DEV)
    #define USE_I2C_EXPANDER false
  #else
    #define USE_I2C_EXPANDER true
  #endif
#endif
#ifndef DEBUG_MODE
  #define DEBUG_MODE         false
#endif

// ── GPIO Pin Map ──────────────────────────────────────────────
#define PIN_LIVING_LIGHT     GPIO_NUM_26
#define PIN_LIVING_FAN       GPIO_NUM_27
#define PIN_BED1_LIGHT       GPIO_NUM_14
#define PIN_BED1_FAN         GPIO_NUM_12
#define PIN_BED2_LIGHT       GPIO_NUM_13
#define PIN_KITCHEN_LIGHT    GPIO_NUM_15
#define PIN_OUTDOOR_LIGHT    GPIO_NUM_33
#define PIN_OUTDOOR_SOCKET   GPIO_NUM_32
#define PIN_GEYSER           GPIO_NUM_25
#define PIN_ALARM_SIREN      GPIO_NUM_23
#define PIN_DOOR_LOCK        GPIO_NUM_4
#define PIN_SERVO_BLINDS     18
#define PIN_STEPPER_IN1      GPIO_NUM_5
#define PIN_STEPPER_IN2      GPIO_NUM_19
#define PIN_STEPPER_IN3      GPIO_NUM_21
#define PIN_STEPPER_IN4      GPIO_NUM_22
#define PIN_IR_SENSOR        GPIO_NUM_34
#define PIN_GAS_ANALOG       GPIO_NUM_35
#define PIN_GAS_DIGITAL      GPIO_NUM_36
#define PIN_DHT_SENSOR       GPIO_NUM_39
#define PIN_POOL_PUMP        GPIO_NUM_2

// LDR (light sensor) + thermistor (analog temp): GPIO37/38 confirmed free
// ADC1 pins on WROVER-E — not PSRAM, not flash, not assigned elsewhere.
// ADC1 stays accurate with WiFi active (unlike ADC2), correct choice here.
#define PIN_LDR               GPIO_NUM_37
#define PIN_THERMISTOR        GPIO_NUM_38
static_assert(PIN_LDR != PIN_THERMISTOR, "PIN_LDR/PIN_THERMISTOR collide.");
// US1 (living-room occupancy ultrasonic).
// IMPORTANT: GPIO16/17 are reserved for PSRAM on ESP32-WROVER modules
// (WROVER-B/E/IE) and cannot be used for any GPIO purpose on those boards —
// using them risks corrupting PSRAM access, not just "not working". Plain
// WROOM-32 (no PSRAM) can use 16/17 freely; WROVER cannot.
// Per the free-GPIO audit (see PIN_US2_TRIG below), classic ESP32 already
// has zero free GPIOs with the base relay/sensor set alone — so on WROVER
// there's no alternate pin pair to give US1 without removing something
// else. US1 is therefore PSRAM-aware: enabled with GPIO16/17 only on
// confirmed non-PSRAM boards, disabled (no GPIO clash, no silent PSRAM
// corruption) everywhere else by default.
#if defined(ARDUINO_ESP32S3_DEV)
  #define PIN_US1_TRIG         GPIO_NUM_16
  #define PIN_US1_ECHO         GPIO_NUM_17
  #define US1_AVAILABLE         1
#elif defined(BOARD_HAS_PSRAM)
  // WROVER-B/E/IE or any classic board with PSRAM enabled: 16/17 unusable,
  // and no free GPIO exists to relocate US1 to.
  #define PIN_US1_TRIG         GPIO_NUM_0   // placeholder, unused while US1_AVAILABLE=0
  #define PIN_US1_ECHO         GPIO_NUM_0
  #define US1_AVAILABLE         0
#else
  // Plain WROOM-32 (no PSRAM): 16/17 are real free GPIO.
  #define PIN_US1_TRIG         GPIO_NUM_16
  #define PIN_US1_ECHO         GPIO_NUM_17
  #define US1_AVAILABLE         1
#endif

// PCF8574 I2C expander — defined here (above the door/window block below) since
// the classic-ESP32 branch needs these as its PIN_DOOR_SENSOR/PIN_WINDOW_SENSOR values.
#define PCF8574_ADDR         0x20
#define PCF8574_PIN_DOOR     0
#define PCF8574_PIN_WINDOW   1

// US3 (gate obstacle ultrasonic) trigger — safe on both boards (strapping pin, fine post-boot).
#define PIN_US3_TRIG         GPIO_NUM_0

#if defined(ARDUINO_ESP32S3_DEV)
  // ── ESP32-S3 branch ────────────────────────────────────────────
  // Free-GPIO audit (quad-SPI S3-WROOM-1 module, no octal PSRAM/flash on
  // GPIO26-37): verified unused by the base 14-device pin set above.
  // Reserved and therefore NOT used below: 19/20 (USB D-/D+), 43/44 (console
  // UART0), 22-25 (not broken out on WROOM-1), 26-37 (octal PSRAM/flash on
  // -N16R8 variants — avoided even on quad-SPI boards for portability).
  #define PIN_US3_ECHO       GPIO_NUM_48  // was GPIO34 — conflicted with PIN_IR_SENSOR
  #define PIN_US2_TRIG       GPIO_NUM_8
  #define PIN_US2_ECHO       GPIO_NUM_9
  #define PIN_DOOR_SENSOR    GPIO_NUM_40
  #define PIN_WINDOW_SENSOR  GPIO_NUM_41
  #define US2_AVAILABLE      1
  #define US3_AVAILABLE      1
#else
  // ── Classic ESP32 (WROOM-32) branch ───────────────────────────
  // Free-GPIO audit: WROOM-32 exposes 26 usable digital GPIOs (0-5, 12-19,
  // 21-23, 25-27, 32-39, excluding 6-11 [flash] and 1/3 [UART0 console]).
  // The base 14-device set above already claims every one of them. There is
  // ZERO free GPIO left on this board for US2, US3, or direct-GPIO
  // door/window sensors — this is a hardware ceiling, not a mapping bug.
  // These features are disabled by default on classic boards. Door/window
  // MUST go through the PCF8574 I2C expander (USE_I2C_EXPANDER=true); US2
  // and US3 obstacle/parking ultrasonics are unavailable on this board
  // without an expander MCU. Do not "free up" a pin by reusing one already
  // assigned above — every classic-board pin is spoken for.
  #define PIN_US2_TRIG       GPIO_NUM_0   // placeholder, unused while US2_AVAILABLE=0
  #define PIN_US2_ECHO       GPIO_NUM_0
  #define US2_AVAILABLE      0
  #define US3_AVAILABLE      0   // no free GPIO for PIN_US3_ECHO on this board

  #if !USE_I2C_EXPANDER
    #error "Classic ESP32 (non-S3): door/window sensors require USE_I2C_EXPANDER=true " \
"(PCF8574 over I2C). Direct-GPIO door/window pins are not available on this " \
"board — every GPIO is already assigned to another sensor or actuator. " \
"Wire a PCF8574 expander and set USE_I2C_EXPANDER=true, or accept that " \
"door/window sensing is unavailable."
  #endif
  #define PIN_DOOR_SENSOR    PCF8574_PIN_DOOR     // expander-only on this board
  #define PIN_WINDOW_SENSOR  PCF8574_PIN_WINDOW
#endif

#if US3_AVAILABLE
static_assert(PIN_US3_ECHO != PIN_POOL_PUMP, "PIN_US3_ECHO conflicts with PIN_POOL_PUMP.");
static_assert(PIN_US3_ECHO != PIN_IR_SENSOR, "PIN_US3_ECHO conflicts with PIN_IR_SENSOR.");
#endif

#if USE_RGB
  // Old default (GPIO25/26/27) collided with PIN_GEYSER/PIN_LIVING_LIGHT/PIN_LIVING_FAN —
  // flashing the status LED would have toggled the geyser and living-room relays.
  // Remapped to dedicated free pins per board (see free-GPIO audit above).
  #if defined(ARDUINO_ESP32S3_DEV)
    #define PIN_RGB_R        GPIO_NUM_1
    #define PIN_RGB_G        GPIO_NUM_3
    #define PIN_RGB_B        GPIO_NUM_6
  #else
    #error "USE_RGB=true on classic ESP32: no free GPIO remains on this board (see " \
"free-GPIO audit above PIN_US2_TRIG). Use an S3 board, drive the indicator " \
"LED over the PCF8574/I2C bus instead, or leave USE_RGB=false."
  #endif
  static_assert(PIN_RGB_R != PIN_GEYSER && PIN_RGB_G != PIN_LIVING_LIGHT && PIN_RGB_B != PIN_LIVING_FAN,
      "PIN_RGB_* still collides with a relay pin — check the assignment above.");
#endif

// ── Timing (ms) ───────────────────────────────────────────────
#define WIFI_TIMEOUT_MS      15000UL
#define WIFI_MAX_RETRIES     3
#define MQTT_RECONNECT_BASE_MS  2000UL
#define MQTT_RECONNECT_MAX_MS   120000UL
#define HEARTBEAT_MS         30000UL
#define SENSOR_READ_MS       10000UL
#define FINGERPRINT_INTERVAL_MS 500UL   // matches "Fingerprint scan (500ms)" cadence
#define GAS_CHECK_MS         3000UL
#define DOOR_CHECK_MS        500UL
#define IR_CHECK_MS          100UL
#define SAFETY_CHECK_MS      10000UL
#define US_READ_MS           5000UL
#define SERIAL_PROV_TIMEOUT_MS 30000UL
#define BLE_TIMEOUT_MS       120000UL
#define SNTP_SYNC_INTERVAL_MS 21600000UL  // 6 hours
#define DEEP_IDLE_THRESHOLD_MS 300000UL

// ── Safety Limits ─────────────────────────────────────────────
#define GEYSER_MAX_MS        14400000UL   // 4 hours
#define PUMP_MAX_MS          1800000UL    // 30 minutes
#define GATE_CONFIRM_MS      3000UL
#define COOLDOWN_MS          500UL
#define GAS_ALERT_THRESHOLD  400
#define TEMP_HIGH_C          35.0f
#define TEMP_LOW_C           5.0f
#define HUMIDITY_HIGH        85.0f

// ── Ultrasonic Thresholds (cm) ────────────────────────────────
#define US_OCCUPANCY_CM          250
#define US_OCCUPANCY_CLEAR_CM    280
#define US_PARKING_TRIGGER_CM    200
#define US_PARKING_CLEAR_CM      240
#define US_GATE_OBSTACLE_CM      30
#define OCCUPANCY_TIMEOUT_MS     30000UL
#define PARKING_ALERT_COOLDOWN   60000UL

// ── Watchdog ──────────────────────────────────────────────────
#define WATCHDOG_TIMEOUT_S   30
#define OTA_WDT_TIMEOUT_S    180    // Extended for OTA download task

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
static const esp_task_wdt_config_t WDT_CONFIG = {
    .timeout_ms    = WATCHDOG_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
};
static const esp_task_wdt_config_t WDT_OTA_CONFIG = {
    .timeout_ms    = OTA_WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
};
#endif

// ── MQTT Broker ───────────────────────────────────────────────
#define MQTT_BROKER          "0ff1c4c0eeec4ff18e9b4be04b5a614b.s1.eu.hivemq.cloud"
#define MQTT_PORT            8883
// FIX (heap sync): was 4096 here, already reduced to 2048 in the
// ArduinoDroid/PC_v3 builds (PubSubClient allocates this from internal
// heap via setBufferSize() — every KB here is a KB not available for TLS).
#define MQTT_BUFFER_SIZE     2048
#define MQTT_KEEPALIVE_S     60

// FIX (heap sync + recalculated): was missing entirely in this build
// despite being referenced in MQTTTransport.h — undefined-identifier
// compile error. Value recalculated for this build's reduced mbedTLS
// buffers (see sdkconfig.defaults) — NOT the same 45000 used in the
// ArduinoDroid/PC_v3 plain-Arduino builds, which still need the larger
// value since they can't reduce mbedTLS's default 32KB record buffers.
#define MQTT_TLS_MIN_FREE_HEAP  22000

// ── NVS Namespaces & Keys ─────────────────────────────────────
#define NVS_NS_IDENTITY      "lmr_identity"
#define NVS_NS_SECRETS       "lmr_secrets"
#define NVS_NS_CONFIG        "lmr_config"
#define NVS_NS_STATE         "lmr_state"
#define NVS_NS_TELEMETRY     "lmr_telem"
#define NVS_NS_DIAG          "lmr_diag"

#define NVS_KEY_UNIT_ID      "unit_id"
#define NVS_KEY_WIFI_SSID    "wifi_ssid"
#define NVS_KEY_WIFI_PASS    "wifi_pass"
#define NVS_KEY_MQTT_USER    "mqtt_user"
#define NVS_KEY_MQTT_PASS    "mqtt_pass"
#define NVS_KEY_DEV_SECRET   "dev_secret"
#define NVS_KEY_LOCAL_TOKEN  "local_token"   // Phase 1 hardening — LAN-only credential, decoupled from dev_secret
#define NVS_KEY_BACKEND_URL  "backend_url"
#define NVS_KEY_AP_PASS      "ap_pass"
#define NVS_KEY_PROV_LOCKED  "prov_locked"
#define NVS_KEY_FW_VERSION   "fw_version"
#define NVS_KEY_PREV_FW      "prev_fw"
#define NVS_KEY_CRASH_CNT    "crash_cnt"

// ── AP Mode ───────────────────────────────────────────────────
#define AP_SSID              "LumaRoK-Setup"
#define AP_PASSWORD_DEFAULT  "LmRk@Setup#2025"
#define AP_PORT              80

// ── Servo LEDC ────────────────────────────────────────────────
#define SERVO_LEDC_CH        0
#define SERVO_FREQ_HZ        50
#define SERVO_BITS           16
#define SERVO_US_MIN         1000
#define SERVO_US_MAX         2000
#define STEPPER_STEPS        512

// ── TLS Certificate (ISRG Root X1) ───────────────────────────
static const char MQTT_ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw
TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh
cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4
WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu
ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY
MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoBggIBAK3oJHP0FDfzm54rVygc
h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+
0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U
A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW
T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH
B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC
B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv
KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn
OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn
jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw
qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI
rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV
HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq
hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL
ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ
3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK
NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5
ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur
TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC
jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc
oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq
4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA
mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d
emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=
-----END CERTIFICATE-----
)EOF";

// ── Serial Baud ───────────────────────────────────────────────
#define SERIAL_BAUD          115200

// ── Crash-loop Thresholds ─────────────────────────────────────
// BUG-5 fix: use named constants instead of magic numbers.
// Rollback fires first (at 3 crashes), before safe mode (at 5).
// This is intentional sequencing — document it explicitly.
#define ROLLBACK_CRASH_THRESHOLD   3
#define SAFE_MODE_CRASH_THRESHOLD  5
#define OFFLINE_TTL_S              1800UL  // OPP-6: discard normal offline readings older than 30 min

// ── Heap Alert Thresholds (soak-test calibrated) ──────────────────────────
// Derived from v3.2 soak test guidance:
//   BLE floor = 82 KB; 60 KB leaves no recovery margin after BLE teardown.
//   Below 40% frag contiguity, large JSON publishes fail silently.
#define HEAP_FREE_WARN_BYTES       (90U * 1024U)   // 90 KB — warn
#define HEAP_FREE_CRIT_BYTES       (60U * 1024U)   // 60 KB — critical (record HEAP_LOW fault)
#define HEAP_FRAG_WARN_PCT         60              // contiguity below 60% = warn
#define HEAP_FRAG_CRIT_PCT         40              // contiguity below 40% = critical
#define HEAP_MIN_DRIFT_WARN_DAY    2048UL          // > 2 KB/day min_free drop = slow-leak warn
#define HEAP_MIN_DRIFT_CRIT_DAY    5120UL          // > 5 KB/day = critical leak

// ── GPIO Conflict Guard (non-S3 targets) ─────────────────────────────────
// PIN_US2_TRIG == PIN_DOOR_LOCK (GPIO4) on non-S3. US2 must be disabled
// (US2_AVAILABLE == 0) in that configuration.
// A build with both active is a silent hardware conflict — make it a linker error.
#if !defined(ARDUINO_ESP32S3_DEV) && defined(US2_AVAILABLE) && (US2_AVAILABLE != 0)
  #error "US2 and DOOR_LOCK share GPIO4 on non-S3 targets. Set US2_AVAILABLE=0 or use an S3 board."
#endif
