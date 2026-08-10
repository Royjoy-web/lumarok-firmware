// ⚠️ DEPRECATED — superseded by the V2 module. Not included in any .ino build (see includes). Kept only for reference; safe to ignore/remove in a future cleanup PR.

#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_task_wdt.h>
#include "../core/Config.h"
#include "../core/Types.h"

// Forward declarations of task entry points (defined in tasks/)
void safetyTaskFn    (void* pvParam);
void sensorTaskFn    (void* pvParam);
void networkTaskFn   (void* pvParam);
void actuatorTaskFn  (void* pvParam);
void mqttRxTaskFn    (void* pvParam);
void mqttTxTaskFn    (void* pvParam);
void otaTaskFn       (void* pvParam);
void diagnosticsTaskFn(void* pvParam);

class TaskManager {
public:
    struct TaskDef {
        TaskFunction_t fn;
        const char*    name;
        uint32_t       stackSize;
        UBaseType_t    priority;
        BaseType_t     core;
    };

    static void startAll() {
        // ── Core 1 (Application CPU) ──────────────────────────
        create({ safetyTaskFn,    "SafetyTask",     4096, 7, 1 });
        create({ sensorTaskFn,    "SensorTask",     4096, 4, 1 });
        create({ actuatorTaskFn,  "ActuatorTask",   4096, 5, 1 });
        // ── Core 0 (Protocol CPU) ─────────────────────────────
        create({ networkTaskFn,   "NetworkTask",    8192, 6, 0 });
        create({ mqttRxTaskFn,    "MQTTRxTask",     4096, 5, 0 });
        create({ mqttTxTaskFn,    "MQTTTxTask",     4096, 4, 0 });
        create({ otaTaskFn,       "OTATask",       16384, 2, 0 });
        create({ diagnosticsTaskFn,"DiagTask",      3072, 1, 0 });
    }

    // ── Watchdog ──────────────────────────────────────────────
    static void initWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        // Arduino core 3.x pre-initialises TWDT; reconfigure instead of re-init
        if (esp_task_wdt_reconfigure(&WDT_CONFIG) != ESP_OK) {
            esp_task_wdt_init(&WDT_CONFIG);  // fallback if not yet init
        }
#else
        esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
#endif
    }

    static void registerCurrentTask() {
        esp_task_wdt_add(NULL);
    }

    static void feedWatchdog() {
        esp_task_wdt_reset();
    }

    static void unregisterCurrentTask() {
        esp_task_wdt_delete(NULL);
    }

    // Reconfigure WDT timeout for the current task (used by OTATask)
    static void extendWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_reconfigure(&WDT_OTA_CONFIG);
#endif
    }

    static void restoreWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_reconfigure(&WDT_CONFIG);
#endif
    }

    // ── Stack high-water diagnostics ──────────────────────────
    static uint32_t stackHighWater(TaskHandle_t task = NULL) {
        return (uint32_t)uxTaskGetStackHighWaterMark(task);
    }

private:
    static void create(const TaskDef& def) {
        TaskHandle_t handle = nullptr;
        BaseType_t rc = xTaskCreatePinnedToCore(
            def.fn, def.name, def.stackSize,
            nullptr, def.priority, &handle, def.core
        );
        if (rc != pdPASS) {
            LOG_E("Tasks", "FAILED to create task: %s", def.name);
        } else {
            LOG_I("Tasks", "Created %s (core %d, pri %d)",
                def.name, (int)def.core, (int)def.priority);
        }
    }
};
