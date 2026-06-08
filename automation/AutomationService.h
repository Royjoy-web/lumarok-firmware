#pragma once
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../automation/DeviceRegistry.h"

// AutomationService — occupancy-driven automation rules.
// Consumes sensor events from EventBus and emits device commands.
// Business logic is isolated here — HAL and MQTT never touched directly.
class AutomationService {
public:
    static void init() {
        LOG_I("Auto", "Automation service initialised");
    }

    // ── Occupancy (US1 — living room) ─────────────────────────
    static void onOccupancy(float distCm) {
        unsigned long now = millis();
        bool detected = (distCm > 0 && distCm < US_OCCUPANCY_CM);

        if (detected) {
            _lastOccupancyMs = now;
            if (!_occupied) {
                _occupied = true;
                LOG_D("Auto", "Occupancy: room occupied — turning on light");
                _sendDeviceCommand("living_room", "light", CommandAction::ON);
            }
        } else {
            // Clear occupancy after timeout
            if (_occupied && (now - _lastOccupancyMs) > OCCUPANCY_TIMEOUT_MS) {
                _occupied = false;
                LOG_D("Auto", "Occupancy: room vacant — turning off light");
                _sendDeviceCommand("living_room", "light", CommandAction::OFF);
            }
        }
    }

    // ── Parking (US2 — outdoor) ───────────────────────────────
    static void onParking(float distCm) {
        unsigned long now = millis();
        bool detected = (distCm > 0 && distCm < US_PARKING_TRIGGER_CM);

        if (detected && !_carPresent) {
            _carPresent   = true;
            _lastParkingMs = now;
            LOG_D("Auto", "Parking: vehicle detected — outdoor light ON");
            _sendDeviceCommand("outdoor", "light", CommandAction::ON);
        }

        if (!detected && _carPresent) {
            _carPresent = false;
            // Alert on departure (rate-limited)
            if ((now - _lastParkingAlertMs) > PARKING_ALERT_COOLDOWN) {
                _lastParkingAlertMs = now;
                Event e{}; e.type = EventType::SAFETY_ALERT;
                strlcpy(e.data.alert.type,    "VEHICLE_DEPARTED", sizeof(e.data.alert.type));
                strlcpy(e.data.alert.message, "Vehicle left parking area", sizeof(e.data.alert.message));
                strlcpy(e.data.alert.device,  "outdoor/parking_us2", sizeof(e.data.alert.device));
                e.data.alert.severity = AlertSeverity::INFO;
                EventBus::postAlert(e);
            }
        }
    }

    // ── IR Beam (gate intrusion) ──────────────────────────────
    static void onIRBeam(bool broken) {
        unsigned long now = millis();
        if (broken && !_irTriggered) {
            _irTriggered     = true;
            _lastIRTriggerMs = now;
            LOG_D("Auto", "IR beam broken — gate intrusion");
            Event e{}; e.type = EventType::SAFETY_ALERT;
            strlcpy(e.data.alert.type,    "GATE_INTRUSION", sizeof(e.data.alert.type));
            strlcpy(e.data.alert.message, "IR beam broken at gate", sizeof(e.data.alert.message));
            strlcpy(e.data.alert.device,  "outdoor/ir_beam", sizeof(e.data.alert.device));
            e.data.alert.severity = AlertSeverity::WARN;
            EventBus::postAlert(e);
        }
        if (!broken) _irTriggered = false;
    }

    static bool isOccupied()  { return _occupied; }
    static bool isCarPresent(){ return _carPresent; }

private:
    static void _sendDeviceCommand(const char* room, const char* name, CommandAction action) {
        Event e{}; e.type = EventType::RELAY_COMMAND;
        strlcpy(e.data.command.room,        room, sizeof(e.data.command.room));
        strlcpy(e.data.command.device_name, name, sizeof(e.data.command.device_name));
        e.data.command.action = action;
        e.data.command.value  = -1;
        EventBus::postCommand(e);
    }

    static bool          _occupied;
    static bool          _carPresent;
    static bool          _irTriggered;
    static unsigned long _lastOccupancyMs;
    static unsigned long _lastParkingMs;
    static unsigned long _lastParkingAlertMs;
    static unsigned long _lastIRTriggerMs;
};

inline bool          AutomationService::_occupied          = false;
inline bool          AutomationService::_carPresent        = false;
inline bool          AutomationService::_irTriggered       = false;
inline unsigned long AutomationService::_lastOccupancyMs   = 0;
inline unsigned long AutomationService::_lastParkingMs     = 0;
inline unsigned long AutomationService::_lastParkingAlertMs= 0;
inline unsigned long AutomationService::_lastIRTriggerMs   = 0;
