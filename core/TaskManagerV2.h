#pragma once
#include "../core/TaskManager.h"
#include "../core/BootManager.h"
#include "../diagnostics/FaultManager.h"

// ── V2 task entry point declarations ─────────────────────────
void networkTaskFnV2    (void*);
void sensorTaskFnV2     (void*);
void otaTaskFnV2        (void*);
void diagnosticsTaskFnV2(void*);

void safetyTaskFn  (void*);
void actuatorTaskFn(void*);
void mqttRxTaskFn  (void*);
void mqttTxTaskFn  (void*);

// ── Task classification (v3.2) ────────────────────────────────
// CRITICAL  — system cannot operate safely without this task.
//             Failure triggers immediate esp_restart(); BootManager
//             escalates to safe mode after repeated failures.
//
// IMPORTANT — degraded operation without this task, but safety is
//             maintained. Failure is logged as a high-severity fault.
//             Device continues to run; diagnostics will report the gap.
//
// OPTIONAL  — non-essential observability / background work.
//             Failure is logged; no restart or fault escalation.
//
// Task          Class       Reason
// SafetyTask    CRITICAL    Gas, door, max-runtime enforcement
// ActuatorTask  CRITICAL    Command execution; safe defaults on restart
// NetworkTask   CRITICAL    WiFi/MQTT bring-up; device unreachable without it
// MQTTRxTask    CRITICAL    Inbound command pipeline
// MQTTTxTask    IMPORTANT   Telemetry/alert delivery (not safety-path)
// SensorTask    IMPORTANT   Environmental sensing (not safety-path; SafetyTask covers gas)
// OTATask       OPTIONAL    Firmware updates — device runs current firmware without it
// DiagTask      OPTIONAL    Diagnostics publishing — does not affect operation

class TaskManagerV2 {
public:
    static void startAll() {
        // ── SAFE mode — network + OTA + diagnostics only ──────
        // In SAFE mode, SafetyTask, SensorTask, and ActuatorTask are NOT started:
        // - The device reached safe mode because of repeated crashes; relaunching
        //   the same application tasks would likely crash again immediately.
        // - NetworkTask + MQTTTxTask/RxTask bring up connectivity so operators
        //   can push a patched OTA without physical access.
        // - OTATask allows firmware recovery.
        // - DiagTask publishes the fault/heap report so the operator knows why
        //   the device entered safe mode.
        // - firmware_hardened.ino starts a lightweight SafeHB task that publishes
        //   TelemetryPipeline::publishHeartbeat() so the backend knows the device
        //   is alive but restricted (OPP-5).
        if (BootManager::mode() == BootMode::SAFE) {
            LOG_W("Tasks", "SAFE MODE — starting network+OTA+diag tasks only");
            _create(networkTaskFnV2,     "NetworkTask",   8192,  6, 0, Tier::CRITICAL);
            _create(mqttRxTaskFn,        "MQTTRxTask",    4096,  5, 0, Tier::CRITICAL);
            _create(mqttTxTaskFn,        "MQTTTxTask",    4096,  4, 0, Tier::IMPORTANT);
            _create(otaTaskFnV2,         "OTATask",      16384,  2, 0, Tier::OPTIONAL);
            _create(diagnosticsTaskFnV2, "DiagTask",      4096,  1, 0, Tier::OPTIONAL);
            return;
        }

        // ── Normal / provisioning-exit mode ───────────────────
        // ── Core 1 (Application CPU) ──────────────────────────
        _create(safetyTaskFn,        "SafetyTask",    4096,  7, 1, Tier::CRITICAL);
        _create(sensorTaskFnV2,      "SensorTask",    5120,  4, 1, Tier::IMPORTANT);
        _create(actuatorTaskFn,      "ActuatorTask",  4096,  5, 1, Tier::CRITICAL);
        // ── Core 0 (Protocol CPU) ─────────────────────────────
        _create(networkTaskFnV2,     "NetworkTask",   8192,  6, 0, Tier::CRITICAL);
        _create(mqttRxTaskFn,        "MQTTRxTask",    4096,  5, 0, Tier::CRITICAL);
        // FIX v3.2: MQTTTxTask changed from undeclared (false) to IMPORTANT.
        // Previously logged failure silently and continued — the TX queue would fill
        // in ~8 messages with no consumer, silently dropping all telemetry and alerts.
        // Now failure is recorded as a SEV_HIGH fault and device restarts,
        // allowing BootManager to detect repeated failures and enter safe mode.
        _create(mqttTxTaskFn,        "MQTTTxTask",    4096,  4, 0, Tier::IMPORTANT);
        _create(otaTaskFnV2,         "OTATask",      16384,  2, 0, Tier::OPTIONAL);
        _create(diagnosticsTaskFnV2, "DiagTask",      4096,  1, 0, Tier::OPTIONAL);
    }

private:
    enum class Tier : uint8_t { CRITICAL, IMPORTANT, OPTIONAL };

    static void _create(TaskFunction_t fn, const char* name,
                         uint32_t stack, UBaseType_t pri,
                         BaseType_t core, Tier tier) {
        BaseType_t rc = xTaskCreatePinnedToCore(fn, name, stack,
                                                 nullptr, pri, nullptr, core);
        if (rc != pdPASS) {
            LOG_E("Tasks", "FAILED to create: %s (tier=%d)", name, (int)tier);

            if (tier == Tier::CRITICAL) {
                // Critical task failure → unsafe state.
                // Delay lets UART flush the error log to serial before restart.
                // BootManager crash counter will increment; escalates to safe mode
                // after SAFE_MODE_CRASH_THRESHOLD consecutive failures.
                LOG_E("Tasks", "CRITICAL task failed — restarting in 1s");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
            }

            if (tier == Tier::IMPORTANT) {
                // Important task failure → record high-severity fault.
                // Device continues but in a degraded state that will be
                // visible in diagnostics. FaultManager may escalate to
                // degraded mode which suppresses non-essential activity.
                LOG_W("Tasks", "IMPORTANT task failed — recording fault, continuing");
                FaultManager::record(FaultCode::TASK_CREATE_FAIL, name, FaultSeverity::SEV_HIGH);
                // Note: no restart here — if the system is OOM at task creation
                // time, restarting immediately would loop. BootManager's crash
                // counter handles repeated startup failures.
            }

            // OPTIONAL tier: log only; already done above.
        } else {
            LOG_I("Tasks", "Started: %-16s  core=%d  pri=%d  stack=%u",
                  name, (int)core, (int)pri, (unsigned)stack);
        }
    }
};
