#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/Identity.h"
#include <Arduino.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../storage/NVSStore.h"

// DeviceRegistry — replaces the static device_map.h C arrays.
// Device list is initialised from compile-time defaults but device IDs
// and state can be updated at runtime via authenticated MQTT command.
//
// THREAD SAFETY MODEL (v3.2)
// ───────────────────────────
// _mutex serialises ALL reads and writes to _devs[] and _sensors[].
// Tasks that touch the registry:
//   ActuatorTask  (Core 1, pri 5) — writes power_state, on_since_ms, last_toggle_ms
//   SafetyTask    (Core 1, pri 7) — reads power_state, on_since_ms, max_runtime_ms
//   SensorTask    (Core 1, pri 4) — reads sensor metadata (read-only after init)
//   NetworkTask   (Core 0, pri 6) — reads device/sensor IDs for topic generation
//   MQTT callback (Core 0, pri 5) — calls updateDeviceId() via command dispatch
//
// Pattern: callers MUST acquire the lock before any read or write.
// Exception: findBy* / device() / sensor() accessors acquire the lock
//            internally and return a COPY so the caller never holds a
//            raw pointer into live data without the lock.
//
// RAII helper: use DeviceRegistry::Lock g; to bracket multi-step operations.

class DeviceRegistry {
public:
    static constexpr int MAX_DEVICES = 16;
    static constexpr int MAX_SENSORS = 12;

    // ── RAII scoped lock ──────────────────────────────────────
    // Usage: { DeviceRegistry::Lock g; /* read or write */ }
    struct Lock {
        Lock()  { xSemaphoreTake(_mutex, portMAX_DELAY); }
        ~Lock() { xSemaphoreGive(_mutex); }
    };

    // ── Explicit lock/unlock (for callers that need granular control) ──
    static void lock()   { xSemaphoreTake(_mutex, portMAX_DELAY); }
    static void unlock() { xSemaphoreGive(_mutex); }

    static void init() {
        _mutex = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        _buildDefaults();
        _restoreDeviceStates();
        LOG_I("Registry", "%d devices, %d sensors loaded", (int)_devCount, (int)_sensorCount);
    }

    // ── Stable accessors (return counts — safe to read without lock
    //    because _devCount/_sensorCount are set once at init and never change) ──
    static int deviceCount()  { return _devCount; }
    static int sensorCount()  { return _sensorCount; }

    // ── Locked pointer accessors — caller MUST hold the lock ──────────
    // These return raw pointers for use inside a Lock{} block only.
    // Do NOT store the pointer and use it after the lock is released.
    static DeviceState* deviceRaw(int i) {
        return (i < _devCount) ? &_devs[i] : nullptr;
    }
    static SensorMeta* sensorRaw(int i) {
        return (i < _sensorCount) ? &_sensors[i] : nullptr;
    }

    // ── Copy-based finders — safe for cross-task reads ────────────────
    // Returns true and fills `out` if found. Caller gets a snapshot;
    // no pointer into live data is exposed.
    static bool findByName(const char* room, const char* name, DeviceState& out) {
        Lock g;
        for (int i = 0; i < _devCount; i++) {
            if (strcmp(_devs[i].room, room) == 0 &&
                strcmp(_devs[i].name, name) == 0) {
                out = _devs[i];
                return true;
            }
        }
        return false;
    }

    static bool findById(const char* id, DeviceState& out) {
        Lock g;
        for (int i = 0; i < _devCount; i++) {
            if (strcmp(_devs[i].device_id, id) == 0) {
                out = _devs[i];
                return true;
            }
        }
        return false;
    }

    static bool findSensorByName(const char* room, const char* name, SensorMeta& out) {
        Lock g;
        for (int i = 0; i < _sensorCount; i++) {
            if (strcmp(_sensors[i].room, room) == 0 &&
                strcmp(_sensors[i].name, name) == 0) {
                out = _sensors[i];
                return true;
            }
        }
        return false;
    }

    // ── Indexed copy — for iteration (e.g. init loops, diagnostics) ──
    static bool getDevice(int i, DeviceState& out) {
        Lock g;
        if (i >= _devCount) return false;
        out = _devs[i];
        return true;
    }
    static bool getSensor(int i, SensorMeta& out) {
        Lock g;
        if (i >= _sensorCount) return false;
        out = _sensors[i];
        return true;
    }

    // ── Atomic state update — the primary write path ──────────────────
    // Finds the device by name, applies the setter lambda, returns false
    // if device not found.  All field writes happen under one lock acquisition.
    // Usage: DeviceRegistry::updateByName("living_room","light",[](DeviceState& d){
    //            d.power_state = true; d.on_since_ms = millis(); });
    template<typename Fn>
    static bool updateByName(const char* room, const char* name, Fn setter) {
        Lock g;
        for (int i = 0; i < _devCount; i++) {
            if (strcmp(_devs[i].room, room) == 0 &&
                strcmp(_devs[i].name, name) == 0) {
                setter(_devs[i]);
                return true;
            }
        }
        return false;
    }

    // ── Device ID update (authenticated MQTT command) ─────────────────
    static bool updateDeviceId(uint8_t gpio, const char* newId) {
        Lock g;                               // FIX: was unlocked in v3.1
        for (int i = 0; i < _devCount; i++) {
            if (_devs[i].gpio == gpio) {
                strlcpy(_devs[i].device_id, newId, sizeof(_devs[i].device_id));
                return true;
            }
        }
        return false;
    }

    // ── State persistence ─────────────────────────────────────────────
    // persistDeviceState: safe to call without holding the lock.
    // ISSUE-3 fix: _indexOfGPIO_nolock() contract required the caller to hold
    // _mutex, but _applyRelay() called this after releasing it. Fix: acquire
    // the lock internally for the brief index lookup.
    static void persistDeviceState(const DeviceState& d) {
        Lock g;
        char key[8]; snprintf(key, sizeof(key), "d%02d", _indexOfGPIO_nolock(d.gpio));
        // NVS write happens under the lock — it's a fast key-value write,
        // not a flash erase (FlashWearGuard coalesces those).
        NVSStore::putBool(NVS_NS_STATE, key, d.power_state);
    }

    static void persistAllStates() {
        // Take a snapshot under the lock, then write NVS without holding it.
        // NVSStore calls are slow (flash) and must not block other tasks
        // that need the registry.
        DeviceState snap[MAX_DEVICES];
        int count;
        {
            Lock g;
            count = _devCount;
            for (int i = 0; i < count; i++) snap[i] = _devs[i];
        }
        for (int i = 0; i < count; i++) persistDeviceState(snap[i]);
    }

    // ── Utility: generate a stable device ID ─────────────────────────
    static void makeId(char* out, size_t len, const char* unitId,
                       const char* room, const char* name) {
        snprintf(out, len, "%s_%s_%s", unitId, room, name);
        for (size_t i = 0; i < len && out[i]; i++)
            if (out[i] == ' ') out[i] = '_';
    }

private:
    static void _buildDefaults() {
        _devCount = 0; _sensorCount = 0;
        const char* uid = Identity::get().c_str();

        char id[48];
        #define MK(suffix) (snprintf(id,sizeof(id),"%s_%s",uid,suffix),(const char*)id)

        _addDevice(MK("living_room_light"),  "living_room",  "light",       "Living Room Light",       PIN_LIVING_LIGHT,    DeviceType::RELAY,   false, 0);
        _addDevice(MK("living_room_fan"),    "living_room",  "fan",         "Living Room Fan",         PIN_LIVING_FAN,      DeviceType::RELAY,   false, 0);
        _addDevice(MK("bedroom1_light"),     "bedroom1",     "light",       "Bedroom 1 Light",         PIN_BED1_LIGHT,      DeviceType::RELAY,   false, 0);
        _addDevice(MK("bedroom1_fan"),       "bedroom1",     "fan",         "Bedroom 1 Fan",           PIN_BED1_FAN,        DeviceType::RELAY,   false, 0);
        _addDevice(MK("bedroom2_light"),     "bedroom2",     "light",       "Bedroom 2 Light",         PIN_BED2_LIGHT,      DeviceType::RELAY,   false, 0);
        _addDevice(MK("kitchen_light"),      "kitchen",      "light",       "Kitchen Light",           PIN_KITCHEN_LIGHT,   DeviceType::RELAY,   false, 0);
        _addDevice(MK("outdoor_light"),      "outdoor",      "light",       "Outdoor Light",           PIN_OUTDOOR_LIGHT,   DeviceType::RELAY,   false, 0);
        _addDevice(MK("outdoor_socket"),     "outdoor",      "socket",      "Outdoor Socket",          PIN_OUTDOOR_SOCKET,  DeviceType::RELAY,   false, 0);
        _addDevice(MK("utility_geyser"),     "utility",      "geyser",      "Geyser",                  PIN_GEYSER,          DeviceType::RELAY,   false, GEYSER_MAX_MS);
        _addDevice(MK("security_siren"),     "security",     "alarm_siren", "Alarm Siren",             PIN_ALARM_SIREN,     DeviceType::RELAY,   false, 0);
        _addDevice(MK("security_door_lock"), "security",     "door_lock",   "Door Lock",               PIN_DOOR_LOCK,       DeviceType::RELAY,   true,  0);
        _addDevice(MK("utility_pool_pump"),  "utility",      "pool_pump",   "Pool Pump",               PIN_POOL_PUMP,       DeviceType::RELAY,   false, PUMP_MAX_MS);
        _addDevice(MK("living_room_blinds"), "living_room",  "blinds",      "Blinds Servo",            PIN_SERVO_BLINDS,    DeviceType::SERVO,   false, 0);
        _addDevice(MK("outdoor_gate"),       "outdoor",      "gate",        "Gate Motor",              PIN_STEPPER_IN1,     DeviceType::STEPPER, false, 0);

        _addSensor(MK("living_room_temperature"), "living_room", "temperature",   "Living Room Temperature", "temperature", PIN_DHT_SENSOR);
        _addSensor(MK("living_room_light"),        "living_room", "light",         "Living Room Light Level", "light",       PIN_LDR);
        _addSensor(MK("living_room_thermistor"),   "living_room", "temperature_aux","Living Room Aux Temp",   "temperature", PIN_THERMISTOR);
        _addSensor(MK("living_room_humidity"),    "living_room", "humidity",      "Living Room Humidity",    "humidity",    PIN_DHT_SENSOR);
        _addSensor(MK("kitchen_gas"),             "kitchen",     "gas",           "Kitchen Gas Detector",    "gas",         PIN_GAS_ANALOG);
        _addSensor(MK("outdoor_ir_beam"),         "outdoor",     "ir_beam",       "Gate IR Beam",            "ir",          PIN_IR_SENSOR);
        _addSensor(MK("security_door"),           "security",    "door",          "Main Door Sensor",        "door",        PCF8574_PIN_DOOR);
        _addSensor(MK("security_window"),         "security",    "window",        "Window Sensor",           "window",      PCF8574_PIN_WINDOW);
#if US1_AVAILABLE
        _addSensor(MK("occupancy_us1"),           "living_room", "occupancy_us1", "Occupancy (US1)",         "occupancy",   PIN_US1_TRIG);
#endif
#if US2_AVAILABLE
        _addSensor(MK("parking_us2"),             "outdoor",     "parking_us2",   "Parking Sensor (US2)",    "proximity",   PIN_US2_TRIG);
#endif
#if US3_AVAILABLE
        _addSensor(MK("gate_obstacle_us3"),       "outdoor",     "gate_obstacle", "Gate Obstacle (US3)",     "proximity",   PIN_US3_TRIG);
#endif

        #undef MK
    }

    static void _addDevice(const char* id, const char* room, const char* name,
                            const char* label, uint8_t gpio,
                            DeviceType type, bool safeDef, uint32_t maxMs) {
        if (_devCount >= MAX_DEVICES) return;
        DeviceState& d  = _devs[_devCount++];
        strlcpy(d.device_id,  id,    sizeof(d.device_id));
        strlcpy(d.room,       room,  sizeof(d.room));
        strlcpy(d.name,       name,  sizeof(d.name));
        strlcpy(d.label,      label, sizeof(d.label));
        d.gpio            = gpio;
        d.type            = type;
        d.safe_default    = safeDef;
        d.power_state     = safeDef;
        d.max_runtime_ms  = maxMs;
        d.last_toggle_ms  = 0;
        d.on_since_ms     = 0;
        d.position        = 0;
    }

    static void _addSensor(const char* id, const char* room, const char* name,
                            const char* label, const char* type, uint8_t gpio) {
        if (_sensorCount >= MAX_SENSORS) return;
        SensorMeta& s = _sensors[_sensorCount++];
        strlcpy(s.sensor_id,   id,    sizeof(s.sensor_id));
        strlcpy(s.room,        room,  sizeof(s.room));
        strlcpy(s.name,        name,  sizeof(s.name));
        strlcpy(s.label,       label, sizeof(s.label));
        strlcpy(s.sensor_type, type,  sizeof(s.sensor_type));
        s.gpio         = gpio;
        s.last_value   = 0;
        s.alert_active = false;
        s.error_count  = 0;
    }

    static void _restoreDeviceStates() {
        // BUG-FIX: on_since_ms must be set for devices restored as ON.
        // SafetyService::_checkMaxRuntimeDevices guards:
        //   if (snap.on_since_ms == 0) continue;
        // Without this fix, a geyser/pump that was ON before a power-cycle
        // boots with power_state=true and on_since_ms=0, and SafetyService
        // silently skips the max-runtime check forever — the geyser can run
        // indefinitely after a reboot. millis() is ~0 at this point in boot,
        // which is conservative (it slightly over-counts runtime), and is
        // corrected the moment the user or automation next toggles the device.
        for (int i = 0; i < _devCount; i++) {
            char key[8]; snprintf(key, sizeof(key), "d%02d", i);
            bool saved = NVSStore::getBool(NVS_NS_STATE, key, _devs[i].safe_default);
            _devs[i].power_state = saved;
            if (saved) _devs[i].on_since_ms = millis();  // start runtime clock
        }
    }

    // No-lock GPIO index lookup — only call when _mutex is already held
    static int _indexOfGPIO_nolock(uint8_t gpio) {
        for (int i = 0; i < _devCount; i++)
            if (_devs[i].gpio == gpio) return i;
        return 0;
    }

    static DeviceState _devs[MAX_DEVICES];
    static SensorMeta  _sensors[MAX_SENSORS];
    static int         _devCount;
    static int         _sensorCount;
    static SemaphoreHandle_t _mutex;
};

inline DeviceState DeviceRegistry::_devs[DeviceRegistry::MAX_DEVICES]       = {};
inline SensorMeta  DeviceRegistry::_sensors[DeviceRegistry::MAX_SENSORS]    = {};
inline int         DeviceRegistry::_devCount    = 0;
inline int         DeviceRegistry::_sensorCount = 0;
inline SemaphoreHandle_t DeviceRegistry::_mutex = nullptr;
