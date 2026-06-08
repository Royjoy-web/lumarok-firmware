#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../diagnostics/WatchdogManager.h"
#include "../mqtt/MQTTTransport.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../diagnostics/CrashLogger.h"
#include "../mqtt/MQTTTopics.h"
#include "../telemetry/TimeSync.h"

// ── MQTTRxTask — Core 0, Pri 5 ───────────────────────────────
// Drives PubSubClient.loop() to pump inbound messages.
// Registered callbacks in CommandDispatcher post to EventBus queues.
// FIX v3.2: Registers with WatchdogManager for stack watermark reporting
// and soft-watchdog visibility. Previously only registered with hardware WDT.
void mqttRxTaskFn(void* pvParam) {
    // 8s timeout — loop() runs every 50ms so missing 160 consecutive ticks
    // indicates a genuine stall (e.g. PubSubClient blocking on TLS read)
    int8_t wdIdx = WatchdogManager::registerTask("MQTTRxTask", 8000);
    LOG_I("MQTTRx", "Running on core %d", (int)xPortGetCoreID());
    for (;;) {
        WatchdogManager::checkin(wdIdx);
        if (EventBus::isMqttConnected()) MQTTTransport::loop();
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// ── MQTTTxTask — Core 0, Pri 4 ───────────────────────────────
// Drains the publish queue at a controlled rate.
// FIX v3.2: Registers with WatchdogManager (was missing — stack watermarks
// were invisible in diagnostics JSON).
// NOTE: MQTTTxTask is classified IMPORTANT (not critical) in TaskManagerV2
// because its failure degrades observability but does not affect safety
// (SafetyTask and ActuatorTask run independently). However on repeated
// creation failure the device will restart and BootManager will escalate.
void mqttTxTaskFn(void* pvParam) {
    int8_t wdIdx = WatchdogManager::registerTask("MQTTTxTask", 8000);
    LOG_I("MQTTTx", "Running on core %d", (int)xPortGetCoreID());
    for (;;) {
        WatchdogManager::checkin(wdIdx);
        if (EventBus::isMqttConnected()) {
            UBaseType_t depth = MQTTTransport::txQueueDepth();
            // FIX v3.2: Previous threshold (> 50) was dead code — MQTT_TX_QUEUE_DEPTH = 16.
            // Now warns at 75% capacity (12/16 slots filled), critical at 100%.
            if (depth >= MQTT_TX_QUEUE_DEPTH) {
                LOG_E("MQTTTx", "TX queue FULL (%u/%d) — dropping telemetry", (unsigned)depth, MQTT_TX_QUEUE_DEPTH);
            } else if (depth >= (MQTT_TX_QUEUE_DEPTH * 3 / 4)) {
                LOG_W("MQTTTx", "TX queue high: %u/%d — broker may be slow or disconnected",
                      (unsigned)depth, MQTT_TX_QUEUE_DEPTH);
            }
            for (int i = 0; i < 8; i++) {
                if (!MQTTTransport::drainOne()) break;
            }
            // Drain alert queue — handles SAFETY_ALERT and RELAY_STATE_CHANGED.
            // OPP-7: ActuatorTask posts RELAY_STATE_CHANGED via EventBus::postAlert()
            // after every relay toggle. Previously only SAFETY_ALERT was consumed here;
            // relay events were silently discarded, making relay history invisible
            // to the backend. This is the consumer side of the OPP-7 completion.
            Event e{};
            while (EventBus::waitAlert(e, 0)) {
                if (e.type == EventType::SAFETY_ALERT) {
                    TelemetryPipeline::publishAlert(e.data.alert);
                } else if (e.type == EventType::RELAY_STATE_CHANGED) {
                    // Compact relay-state JSON: device, gpio, state, source, ts.
                    // QoS 0, not retained — backend time-series captures history;
                    // retaining would serve stale state to freshly connected clients.
                    char rbuf[128];
                    snprintf(rbuf, sizeof(rbuf),
                             "{\"device\":\"%s\",\"gpio\":%u,\"state\":%s,\"src\":\"%s\",\"ts\":%lu}",
                             e.data.relay.device_id,
                             (unsigned)e.data.relay.gpio,
                             e.data.relay.state ? "true" : "false",
                             e.data.relay.source,
                             (unsigned long)TimeSync::bestEffort());
                    MQTTTransport::publish(MQTTTopics::relayState(), rbuf, 0, false);
                }
                // All other types reaching postAlert() are EventGroup-handled (OPP-4);
                // dropping here is correct — they should never arrive.
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
