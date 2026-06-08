#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../ota/OTAManager.h"
#include "../diagnostics/DiagnosticsManager.h"
#include "../diagnostics/DiagnosticsManager.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../core/Config.h"

// ── OTATask — Core 0, Pri 2 ───────────────────────────────────
// Dormant until an OTA_COMMAND event arrives.
// Runs with extended watchdog timeout during download.
void otaTaskFn(void* pvParam) {
    TaskManager::registerCurrentTask();
    OTAManager::init();
    LOG_I("OTATask", "Ready (dormant)");

    for (;;) {
        TaskManager::feedWatchdog();

        // Block on OTA queue — wakes only when backend sends OTA command
        Event e{};
        if (!EventBus::waitOTA(e, pdMS_TO_TICKS(5000))) continue;
        if (e.type != EventType::OTA_COMMAND) continue;

        LOG_I("OTATask", "OTA command received — starting");

        // Extend watchdog for download duration
        TaskManager::extendWatchdog();

        OTAManager::begin(e.data.ota);

        // Drive state machine to completion
        while (!OTAManager::tick()) {
            TaskManager::feedWatchdog();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        // Restore normal watchdog (only reached on failure; success reboots)
        TaskManager::restoreWatchdog();
        LOG_I("OTATask", "OTA finished (state=%d)", (int)OTAManager::state());
    }
}

// ── DiagnosticsTask — Core 0, Pri 1 ──────────────────────────
// Lowest-priority background task: heap monitoring, metrics publish,
// telemetry buffer drain after reconnect.
void diagnosticsTaskFn(void* pvParam) {
    TaskManager::registerCurrentTask();
    DiagnosticsManager::init();

    unsigned long lastDiagMs   = 0;
    unsigned long lastDrainMs  = 0;
    const unsigned long DIAG_INTERVAL  = 60000UL;
    const unsigned long DRAIN_INTERVAL = 10000UL;

    LOG_I("DiagTask", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        TaskManager::feedWatchdog();
        HeapMonitor::tick();

        unsigned long now = millis();

        if (EventBus::isMqttConnected()) {
            // Publish full diagnostics every 60s
            if (now - lastDiagMs >= DIAG_INTERVAL) {
                lastDiagMs = now;
                DiagnosticsManager::publishDiagnostics();
            }
            // Drain offline telemetry buffer every 10s
            if (now - lastDrainMs >= DRAIN_INTERVAL) {
                lastDrainMs = now;
                TelemetryPipeline::drainBuffer(3);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
