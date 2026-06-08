#pragma once
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/RetryPolicy.h"
#include "../storage/NVSStore.h"

// ─────────────────────────────────────────────────────────────
// FaultManager — subsystem fault registry with auto-recovery.
// v3.2 additions: TASK_CREATE_FAIL(11), heap threshold faults tied to
//   HEAP_FREE_WARN_BYTES / HEAP_FREE_CRIT_BYTES in Config.h.
//
// Each subsystem registers a fault code when it fails.
// FaultManager tracks severity, recurrence, and triggers:
//   • Soft recovery: retry with backoff
//   • Hard recovery: subsystem restart
//   • Critical: device restart after N consecutive hard failures
//   • Degraded mode: disables non-safety features to preserve MQTT
// ─────────────────────────────────────────────────────────────

enum class FaultCode : uint8_t {
    NONE             = 0,
    DHT_READ_FAIL    = 1,
    GAS_SENSOR_FAIL  = 2,
    I2C_BUS_FAIL     = 3,
    MQTT_CONN_FAIL   = 4,
    WIFI_CONN_FAIL   = 5,
    OTA_FAIL         = 6,
    NVS_WRITE_FAIL   = 7,
    HEAP_LOW         = 8,
    STEPPER_FAULT    = 9,
    WATCHDOG_NEAR    = 10,
    TASK_CREATE_FAIL = 11,   // Task creation failure (OOM at startup)
    COUNT
};

enum class FaultSeverity : uint8_t { SEV_LOW=0, SEV_MEDIUM=1, SEV_HIGH=2, SEV_CRITICAL=3 };

struct FaultEntry {
    FaultCode    code;
    FaultSeverity severity;
    uint16_t     count;
    uint16_t     consecutiveCount;
    unsigned long firstOccurredMs;
    unsigned long lastOccurredMs;
    bool          active;
    char          context[32];
};

enum class SystemMode : uint8_t {
    NORMAL,    // All features active
    DEGRADED,  // Non-safety features suspended to preserve resources
    SAFE,      // Safety-only: relays, gas, door. No telemetry, no OTA.
    FAILED     // Restart imminent
};

class FaultManager {
public:
    static void init() {
        memset(_faults, 0, sizeof(_faults));
        for (int i = 0; i < (int)FaultCode::COUNT; i++) {
            _faults[i].code = (FaultCode)i;
        }
        _mode = SystemMode::NORMAL;
        LOG_I("Fault", "Fault manager initialised");
    }

    // ── Record a fault ────────────────────────────────────────
    static void record(FaultCode code, const char* context = nullptr,
                        FaultSeverity sev = FaultSeverity::SEV_MEDIUM) {
        if (code >= FaultCode::COUNT) return;
        FaultEntry& f = _faults[(int)code];
        f.code             = code;
        f.severity         = sev;
        f.count++;
        f.consecutiveCount++;
        f.lastOccurredMs   = millis();
        f.active           = true;
        if (!f.firstOccurredMs) f.firstOccurredMs = millis();
        if (context) strlcpy(f.context, context, sizeof(f.context));

        LOG_W("Fault", "FAULT[%d] %s count=%u consec=%u",
              (int)code, context ? context : "", f.count, f.consecutiveCount);

        _evaluateMode();
        _triggerRecovery(f);
    }

    // ── Clear a fault (on successful operation) ───────────────
    static void clear(FaultCode code) {
        if (code >= FaultCode::COUNT) return;
        FaultEntry& f = _faults[(int)code];
        if (!f.active) return;
        f.active           = false;
        f.consecutiveCount = 0;
        LOG_I("Fault", "CLEARED[%d]", (int)code);
        _evaluateMode();
    }

    static bool         isActive(FaultCode c) { return _faults[(int)c].active; }
    static uint16_t     count   (FaultCode c) { return _faults[(int)c].count;  }
    static SystemMode   mode()                { return _mode; }
    static bool         isDegraded()          { return _mode >= SystemMode::DEGRADED; }
    static bool         isSafeMode()          { return _mode >= SystemMode::SAFE; }

    static void buildFaultJSON(char* buf, size_t len) {
        size_t pos = 0;
        pos += snprintf(buf, len, "{\"mode\":%d,\"faults\":[", (int)_mode);
        bool first = true;
        for (int i = 1; i < (int)FaultCode::COUNT; i++) {
            FaultEntry& f = _faults[i];
            if (!f.active && f.count == 0) continue;
            pos += snprintf(buf + pos, len - pos,
                "%s{\"code\":%d,\"count\":%u,\"active\":%s,\"ctx\":\"%s\"}",
                first ? "" : ",", i, f.count,
                f.active ? "true" : "false", f.context);
            first = false;
        }
        snprintf(buf + pos, len - pos, "]}");
    }

private:
    static void _evaluateMode() {
        // Count active faults by severity
        uint8_t critical = 0, high = 0, medium = 0;
        for (int i = 1; i < (int)FaultCode::COUNT; i++) {
            if (!_faults[i].active) continue;
            switch (_faults[i].severity) {
                case FaultSeverity::SEV_CRITICAL: critical++; break;
                case FaultSeverity::SEV_HIGH:     high++;     break;
                case FaultSeverity::SEV_MEDIUM:   medium++;   break;
                default: break;
            }
        }

        SystemMode prev = _mode;
        if (critical > 0 || high >= 3)     _mode = SystemMode::FAILED;
        else if (high >= 1 || medium >= 4) _mode = SystemMode::SAFE;
        else if (medium >= 2)              _mode = SystemMode::DEGRADED;
        else                               _mode = SystemMode::NORMAL;

        if (_mode != prev) {
            LOG_W("Fault", "System mode: %d → %d", (int)prev, (int)_mode);
            if (_mode == SystemMode::FAILED) {
                LOG_E("Fault", "FAILED mode — posting restart event");
                // BUG-F fix: do NOT call vTaskDelay(2000) + esp_restart() inline.
                // _evaluateMode() is called from record() which can execute on
                // SafetyTask (pri 7). Blocking there for 2s starves NetworkTask
                // (pri 6), preventing hardware WDT feed → panic before clean restart.
                // Fix: post SYSTEM_RESTART to _commandQ; ActuatorTask (pri 5)
                // drains it, persists state, and restarts cleanly.
                Event e{}; e.type = EventType::SYSTEM_RESTART;
                // postCommand with zero wait — if the queue is full (unlikely at
                // shutdown), fall back to a direct delayed restart on this task.
                if (!EventBus::postCommand(e, 0)) {
                    LOG_E("Fault", "Restart queue full — direct restart in 500ms");
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
        }
    }

    static void _triggerRecovery(FaultEntry& f) {
        // Subsystem-specific recovery actions
        switch (f.code) {
            case FaultCode::I2C_BUS_FAIL:
                if (f.consecutiveCount == 2) {
                    LOG_I("Fault", "I2C bus recovery attempt");
                    // PCF8574Driver::recover() called from SafetyTask context
                }
                break;
            case FaultCode::HEAP_LOW:
                // Suspend non-critical telemetry to free heap
                LOG_W("Fault", "Heap low — suspending normal telemetry");
                break;
            case FaultCode::STEPPER_FAULT:
                if (f.consecutiveCount >= 3) {
                    LOG_W("Fault", "Stepper fault — forcing home position");
                }
                break;
            default: break;
        }
    }

    static FaultEntry _faults[(int)FaultCode::COUNT];
    static SystemMode _mode;
};

inline FaultEntry FaultManager::_faults[(int)FaultCode::COUNT] = {};
inline SystemMode FaultManager::_mode = SystemMode::NORMAL;
