#pragma once
#include <Arduino.h>
#include <time.h>
#include <stdint.h>

// ── Device Types ──────────────────────────────────────────────
enum class DeviceType : uint8_t { RELAY=0, SERVO=1, STEPPER=2, SENSOR=3 };

struct DeviceState {
    char      device_id[32];
    char      room[24];
    char      name[32];
    char      label[48];
    uint8_t   gpio;
    DeviceType type;
    bool      power_state;
    bool      safe_default;
    unsigned long last_toggle_ms;
    unsigned long on_since_ms;
    uint32_t  max_runtime_ms;
    int       position;    // servo degrees / stepper step count
};

struct SensorMeta {
    char     sensor_id[32];
    char     room[24];
    char     name[32];
    char     label[48];
    char     sensor_type[16];
    uint8_t  gpio;
    float    last_value;
    bool     alert_active;
    uint16_t error_count;
};

// ── Telemetry ─────────────────────────────────────────────────
struct SensorReading {
    char   sensor_id[32];
    char   room[24];
    char   name[16];
    char   unit[8];
    float  value;
    time_t timestamp;
    bool   ts_uncertain;
    uint16_t seq;
    bool   quality_ok;
    uint8_t  error_count;
};

// ── Commands ──────────────────────────────────────────────────
enum class CommandAction : uint8_t {
    ON=0, OFF=1, TOGGLE=2, SET_VALUE=3,
    RESTART=4, ROTATE_CREDS=5, RESET_PROV=6
};

struct DeviceCommand {
    char          device_id[32];
    char          room[24];
    char          device_name[32];
    CommandAction action;
    int           value;    // servo position or -1
};

struct CredRotateCommand {
    char mqtt_user[64];
    char mqtt_pass[64];
    char dev_secret[64];
    char sig[65];
    long ts;
};

// Phase 1 hardening — local_token pairing. Delivered over its own MQTT
// topic (LocalTokenProvisioner), signed with dev_secret same as cred-rotate,
// but only ever touches local_token — never mqtt creds or dev_secret itself.
struct LocalTokenCommand {
    char token[65];
    char sig[65];
    long ts;
};

// ── OTA ───────────────────────────────────────────────────────
struct OTACommand {
    char url[256];
    char version[32];
    char sha256[65];
    char token[65];
    char sig[65];
    long ts;
};

// ── Alerts ────────────────────────────────────────────────────
enum class AlertSeverity : uint8_t { INFO=0, WARN=1, CRITICAL=2 };

struct Alert {
    char          type[32];
    char          message[96];
    char          device[48];
    AlertSeverity severity;
    time_t        timestamp;
};

// ── Event Bus ─────────────────────────────────────────────────
enum class EventType : uint8_t {
    SENSOR_READING,
    RELAY_COMMAND,
    RELAY_STATE_CHANGED,
    SERVO_COMMAND,
    STEPPER_COMMAND,
    SAFETY_ALERT,
    MQTT_CONN_OK,
    MQTT_DISCONNECTED,
    WIFI_CONNECTED,
    WIFI_DISCONNECTED,
    COMMAND_RECEIVED,
    OTA_COMMAND,
    OTA_PROGRESS,
    CRED_ROTATE_COMMAND,
    LOCAL_TOKEN_ROTATE_COMMAND,
    SYSTEM_RESTART,
    HEARTBEAT_TICK,
    CRASH_RECOVERED,
    HEAP_LOW,
    TIME_SYNCED
};

struct NetworkInfo  { char ip[16]; char ssid[32]; int rssi; };
struct MQTTStatus   { int  rc; };
struct OTAProgress  { int  percent; char status[32]; char reason[32]; };
struct RelayEvent   { char device_id[32]; uint8_t gpio; bool state; char source[16]; };
struct ActuatorCmd  { char device_id[32]; char room[24]; char name[32]; int value; };

struct Event {
    EventType type;
    union {
        SensorReading       sensor;
        DeviceCommand       command;
        OTACommand          ota;
        CredRotateCommand   cred_rotate;
        LocalTokenCommand   local_token;
        Alert               alert;
        NetworkInfo         net;
        MQTTStatus          mqtt_status;
        OTAProgress         ota_progress;
        RelayEvent          relay;
        ActuatorCmd         actuator;
        uint32_t            value_u32;
    } data;
};

// ── Diagnostics ───────────────────────────────────────────────
struct CrashRecord {
    uint32_t crash_reason;
    char     task_name[16];
    char     last_topic[64];
    uint32_t uptime_s;
    uint32_t free_heap;
    uint8_t  crash_count;
    bool     valid;
};

// ── Log Levels ────────────────────────────────────────────────
#define LOG_LEVEL_NONE   0
#define LOG_LEVEL_ERROR  1
#define LOG_LEVEL_WARN   2
#define LOG_LEVEL_INFO   3
#define LOG_LEVEL_DEBUG  4

#ifndef LUMAROK_LOG_LEVEL
  #define LUMAROK_LOG_LEVEL LOG_LEVEL_INFO
#endif

#if LUMAROK_LOG_LEVEL >= LOG_LEVEL_DEBUG
  #define LOG_D(tag, fmt, ...) Serial.printf("[D][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
  #define LOG_D(tag, fmt, ...) (void)0
#endif

#if LUMAROK_LOG_LEVEL >= LOG_LEVEL_INFO
  #define LOG_I(tag, fmt, ...) Serial.printf("[I][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
  #define LOG_I(tag, fmt, ...) (void)0
#endif

#if LUMAROK_LOG_LEVEL >= LOG_LEVEL_WARN
  #define LOG_W(tag, fmt, ...) Serial.printf("[W][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
  #define LOG_W(tag, fmt, ...) (void)0
#endif

#if LUMAROK_LOG_LEVEL >= LOG_LEVEL_ERROR
  #define LOG_E(tag, fmt, ...) Serial.printf("[E][%s] " fmt "\n", tag, ##__VA_ARGS__)
#else
  #define LOG_E(tag, fmt, ...) (void)0
#endif
