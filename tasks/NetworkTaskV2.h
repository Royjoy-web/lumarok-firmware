#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../core/Config.h"
#include "../core/BootManager.h"
#include "../networking/ReconnectEngine.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../mqtt/CommandDispatcher.h"
#include "../mqtt/LocalTokenProvisioner.h"
#include "../networking/LocalCommandServer.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../telemetry/BatchPublisher.h"
#include "../diagnostics/DiagnosticsManager.h"
#include "../diagnostics/WatchdogManager.h"
#include "../diagnostics/FaultManager.h"
#include "../storage/FlashWearGuard.h"

// Replaces NetworkTask.h — uses ReconnectEngine instead of WiFiManager.
void networkTaskFnV2(void* pvParam) {
    int8_t wdIdx = WatchdogManager::registerTask("NetworkTask", 15000);

    LOG_I("HeapDiag", "Internal free at NetworkTask entry: %u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    // FIX (heap audit): all 8 tasks allocate their stacks/queues/mutexes
    // concurrently at boot — that's the exact moment free internal heap is
    // at its lowest, which is also when NetworkTask used to fire its first
    // TLS attempt. A short settle delay moves the first MQTT connect() past
    // that transient low-water mark. 15000ms WDM timeout gives ample margin.
    vTaskDelay(pdMS_TO_TICKS(3000));

    LOG_I("HeapDiag", "Internal free after 3s settle delay: %u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    ReconnectEngine::init();
    MQTTTransport::init();
    MQTTTopics::init(Identity::get());
    BatchPublisher::init();

    LOG_I("HeapDiag", "Internal free after MQTT/BatchPublisher init: %u",
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    bool   subsRegistered  = false;
    bool   stableConfirmed = false;
    bool   localSrvUp      = false;
    unsigned long lastHeartbeatMs  = 0;
    unsigned long lastStableCheckMs= 0;
    unsigned long stableStartMs    = 0;

    LOG_I("NetTaskV2", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        WatchdogManager::checkin(wdIdx);

        // ── Drive reconnect state machine ─────────────────────
        ReconnectEngine::tick();

        bool wifiOk = ReconnectEngine::wifiConnected();
        bool mqttOk = ReconnectEngine::mqttConnected();

        // ── Local control path (Phase 1) ───────────────────────
        // Deliberately keyed off WiFi, not MQTT: this is the whole point
        // of the local path — it must keep serving commands on the LAN
        // through cloud/internet outages, not just when MQTT is up.
        if (wifiOk && !localSrvUp) {
            LocalCommandServer::begin();
            localSrvUp = true;
        } else if (!wifiOk && localSrvUp) {
            LocalCommandServer::end();
            localSrvUp = false;
        }
        if (localSrvUp) LocalCommandServer::tick();

        // ── Post-connect init (once per reconnect cycle) ──────
        if (mqttOk && !subsRegistered) {
            subsRegistered = true;
            CommandDispatcher::registerSubscriptions();
            LocalTokenProvisioner::registerSubscriptions();
            DiagnosticsManager::publishCrashRecord();
            BatchPublisher::drainOffline(5);
            stableStartMs = millis();
            LOG_I("NetTaskV2", "Subscriptions registered, buffer draining");
        }

        if (!mqttOk) {
            subsRegistered = false;
            stableConfirmed = false;
            stableStartMs   = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // ── Heartbeat ─────────────────────────────────────────
        unsigned long now = millis();
        if (now - lastHeartbeatMs >= HEARTBEAT_MS) {
            lastHeartbeatMs = now;
            // Degraded mode: include fault status in heartbeat
            if (FaultManager::isDegraded()) {
                char faultBuf[256];
                FaultManager::buildFaultJSON(faultBuf, sizeof(faultBuf));
                MQTTTransport::publish(MQTTTopics::diagnostics(), faultBuf, 1, false);
            }
            TelemetryPipeline::publishHeartbeat();
            FlashWearGuard::checkBudget();
        }

        // ── Stable operation confirmation (5 min uptime) ──────
        if (!stableConfirmed && stableStartMs > 0) {
            if (now - stableStartMs >= 300000UL) {
                stableConfirmed = true;
                BootManager::confirmStableOperation();
                LOG_I("NetTaskV2", "Stable operation confirmed");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
