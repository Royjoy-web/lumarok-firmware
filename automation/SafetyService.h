#pragma once
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../automation/DeviceRegistry.h"
#include "../telemetry/TimeSync.h"

// SafetyService — runs in SafetyTask at highest priority (Core 1, pri 7).
// Enforces hardware safety limits regardless of MQTT connectivity.
// Never depends on MQTTTransport directly; posts Alerts to EventBus.
//
// THREAD SAFETY (v3.2):
//   All DeviceRegistry access now uses the copy-based and RAII-locked APIs.
//   _checkMaxRuntimeDevices: reads device state under lock (was unprotected in v3.1)
//   _safeShutoffGroup:       reads device state under lock (was unprotected in v3.1)
class SafetyService {
public:
    static void init() {
        LOG_I("Safety", "Safety service initialised");
    }

    // Call every SAFETY_CHECK_MS from SafetyTask
    static void tick(unsigned long now) {
        _checkMaxRuntimeDevices(now);
    }

    // ── Gas Detection ─────────────────────────────────────────
    static void onGasReading(int analogVal, bool digitalAlert) {
        bool triggered = (analogVal >= GAS_ALERT_THRESHOLD) || digitalAlert;

        if (triggered && !_gasAlertActive) {
            _gasAlertActive = true;
            LOG_W("Safety", "GAS DETECTED — analog=%d digital=%d", analogVal, (int)digitalAlert);
            _safeShutoffGroup("utility");
            _postAlert("GAS_DETECTED", "Gas concentration above threshold", "kitchen/gas", AlertSeverity::CRITICAL);
        }

        if (!triggered && _gasAlertActive) {
            _gasAlertActive = false;
            _postAlert("GAS_CLEARED", "Gas levels returned to normal", "kitchen/gas", AlertSeverity::INFO);
            LOG_I("Safety", "Gas alert cleared");
        }
    }

    // ── Door / Window ─────────────────────────────────────────
    static void onDoorState(bool open, bool& prevState) {
        if (open == prevState) return;
        prevState = open;
        if (open) _postAlert("DOOR_OPENED", "Main door opened", "security/door", AlertSeverity::WARN);
    }

    static void onWindowState(bool open, bool& prevState) {
        if (open == prevState) return;
        prevState = open;
        if (open) _postAlert("WINDOW_OPENED", "Window opened", "security/window", AlertSeverity::WARN);
    }

    // ── Temperature ───────────────────────────────────────────
    static void onTemperature(float t) {
        if (t > TEMP_HIGH_C && !_tempHighActive) {
            _tempHighActive = true;
            _postAlert("TEMP_HIGH", "Temperature exceeds safe limit", "living_room/temperature", AlertSeverity::WARN);
        } else if (t <= TEMP_HIGH_C) {
            _tempHighActive = false;
        }
    }

    // ── Gate obstacle ─────────────────────────────────────────
    static bool isGateObstacle(float distCm) {
        return (distCm > 0 && distCm < US_GATE_OBSTACLE_CM);
    }

    static bool gasAlertActive()  { return _gasAlertActive; }
    static bool tempHighActive()  { return _tempHighActive; }

private:
    // FIX v3.2: reads device state under DeviceRegistry lock.
    // Previously iterated _devs[] directly without the mutex, allowing
    // SafetyTask (pri 7) to read partially-written state while ActuatorTask
    // (pri 5, same core) was mid-update of power_state / on_since_ms.
    static void _checkMaxRuntimeDevices(unsigned long now) {
        int count = DeviceRegistry::deviceCount();
        for (int i = 0; i < count; i++) {
            DeviceState snap;
            if (!DeviceRegistry::getDevice(i, snap)) continue;
            if (!snap.power_state || snap.max_runtime_ms == 0) continue;
            if (snap.on_since_ms == 0) continue;
            if ((now - snap.on_since_ms) >= snap.max_runtime_ms) {
                LOG_W("Safety", "Max runtime reached: %s/%s — forcing off", snap.room, snap.name);
                Event e{}; e.type = EventType::RELAY_COMMAND;
                strlcpy(e.data.command.room,        snap.room, sizeof(e.data.command.room));
                strlcpy(e.data.command.device_name, snap.name, sizeof(e.data.command.device_name));
                e.data.command.action = CommandAction::OFF;
                EventBus::postCommand(e);

                char msg[80];
                snprintf(msg, sizeof(msg), "%s max runtime exceeded", snap.label);
                _postAlert("MAX_RUNTIME", msg, snap.name, AlertSeverity::WARN);
            }
        }
    }

    // BUG-C fix: do NOT post commands while holding DeviceRegistry::Lock.
    // If _commandQ is full (16 slots), xQueueSend blocks for up to 50ms
    // while ActuatorTask waits on waitCommand — which calls findByName()
    // which tries to acquire _mutex. Classic deadlock on the gas-alert path.
    //
    // Fix: snapshot room/name under the lock, release it, then post.
    static void _safeShutoffGroup(const char* room) {
        // Snapshot room/name pairs under the lock — 16 × 56 bytes is too
        // large for the SafetyTask stack; store only small name buffers.
        struct NamePair { char room[24]; char name[32]; };
        NamePair pairs[DeviceRegistry::MAX_DEVICES];
        int targetCount = 0;

        {
            DeviceRegistry::Lock g;
            int count = DeviceRegistry::deviceCount();
            for (int i = 0; i < count && targetCount < DeviceRegistry::MAX_DEVICES; i++) {
                DeviceState* d = DeviceRegistry::deviceRaw(i);
                if (d && strcmp(d->room, room) == 0) {
                    strlcpy(pairs[targetCount].room, d->room, sizeof(pairs[0].room));
                    strlcpy(pairs[targetCount].name, d->name, sizeof(pairs[0].name));
                    targetCount++;
                }
            }
        }   // ← lock released here, before any postCommand call

        for (int i = 0; i < targetCount; i++) {
            Event e{}; e.type = EventType::RELAY_COMMAND;
            strlcpy(e.data.command.room,        pairs[i].room, sizeof(e.data.command.room));
            strlcpy(e.data.command.device_name, pairs[i].name, sizeof(e.data.command.device_name));
            e.data.command.action = CommandAction::OFF;
            EventBus::postCommand(e);
        }
    }

    static void _postAlert(const char* type, const char* msg,
                            const char* device, AlertSeverity sev) {
        Event e{}; e.type = EventType::SAFETY_ALERT;
        strlcpy(e.data.alert.type,    type,   sizeof(e.data.alert.type));
        strlcpy(e.data.alert.message, msg,    sizeof(e.data.alert.message));
        strlcpy(e.data.alert.device,  device, sizeof(e.data.alert.device));
        e.data.alert.severity  = sev;
        e.data.alert.timestamp = TimeSync::bestEffort();
        EventBus::postAlert(e);
    }

    static bool _gasAlertActive;
    static bool _tempHighActive;
};

inline bool SafetyService::_gasAlertActive = false;
inline bool SafetyService::_tempHighActive = false;
