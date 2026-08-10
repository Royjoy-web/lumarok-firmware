// ⚠️ DEPRECATED — superseded by the V2 module. Not included in any .ino build (see includes). Kept only for reference; safe to ignore/remove in a future cleanup PR.

#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../core/Config.h"
#include "../networking/WiFiManager.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../mqtt/CommandDispatcher.h"
#include "../diagnostics/DiagnosticsManager.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../core/BootManager.h"

void networkTaskFn(void* pvParam) {
    TaskManager::registerCurrentTask();

    WiFiManager::init();
    MQTTTransport::init();
    MQTTTopics::init(Identity::get());

    bool mqttSubsRegistered = false;
    unsigned long lastHeartbeatMs = 0;
    unsigned long stableStartMs   = 0;
    bool stabilityConfirmed       = false;

    LOG_I("NetworkTask", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        TaskManager::feedWatchdog();

        // ── WiFi ──────────────────────────────────────────────
        bool wifiOk = WiFiManager::tick();

        if (!wifiOk) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // ── MQTT ──────────────────────────────────────────────
        if (!MQTTTransport::isConnected()) {
            mqttSubsRegistered = false;
            stableStartMs      = 0;
            stabilityConfirmed = false;
            MQTTTransport::connect(); // non-blocking with backoff
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        // ── Post-connect setup (once per reconnect) ────────────
        if (!mqttSubsRegistered) {
            mqttSubsRegistered = true;
            CommandDispatcher::registerSubscriptions();
            DiagnosticsManager::publishCrashRecord();
            TelemetryPipeline::drainBuffer();
            LOG_I("NetworkTask", "MQTT subscriptions registered");
        }

        // ── Heartbeat ─────────────────────────────────────────
        unsigned long now = millis();
        if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
            lastHeartbeatMs = now;
            TelemetryPipeline::publishHeartbeat();
        }

        // ── Stable operation confirmation (5 min) ─────────────
        if (!stabilityConfirmed) {
            if (stableStartMs == 0) stableStartMs = now;
            if (now - stableStartMs >= 300000UL) {
                stabilityConfirmed = true;
                BootManager::confirmStableOperation();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
