#pragma once
#include "../core/Types.h"
#include "../core/EventBus.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../core/Identity.h"
#include "../diagnostics/CrashLogger.h"
#include "../storage/TelemetryBuffer.h"
#include "../telemetry/TimeSync.h"
#include "../networking/WiFiManager.h"

#include "HeapMonitor.h"

// ── DiagnosticsManager ────────────────────────────────────────
class DiagnosticsManager {
public:
    static void init() {
        CrashLogger::init();
        if (CrashLogger::hasCrashRecord()) {
            _pendingCrash = true;
            LOG_W("Diag", "Previous crash record found");
        }
    }

    // Publish crash record if present — called after MQTT connects
    static void publishCrashRecord() {
        if (!_pendingCrash) return;
        const CrashRecord& r = CrashLogger::lastCrash();
        char buf[320];
        snprintf(buf, sizeof(buf),
            "{\"event\":\"crash_recovery\",\"reason\":%u,\"task\":\"%s\","
            "\"last_topic\":\"%s\",\"uptime_s\":%u,\"heap\":%u,"
            "\"crash_count\":%u,\"unit_id\":\"%s\"}",
            r.crash_reason, r.task_name, r.last_topic,
            r.uptime_s, r.free_heap, r.crash_count,
            Identity::get().c_str());
        MQTTTransport::publish(MQTTTopics::diagnostics(), buf, 1, false);
        CrashLogger::clearCrashRecord();
        _pendingCrash = false;
    }

    // Full diagnostics publish — every 60s from DiagnosticsTask
    static void publishDiagnostics() {
        char buf[640];
        snprintf(buf, sizeof(buf),
            "{\"unit_id\":\"%s\",\"fw\":\"%s\",\"uptime_s\":%lu,"
            "\"heap_free\":%u,\"heap_min\":%u,\"heap_largest\":%u,"
            "\"wifi_rssi\":%d,\"mqtt_reconnects\":%u,"
            "\"telem_buf\":%u,\"tx_queue\":%u,\"ts\":%lu}",
            Identity::get().c_str(), FIRMWARE_VERSION,
            millis() / 1000UL,
            HeapMonitor::free(), HeapMonitor::minFree(),
            (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
            WiFiManager::rssi(),
            (unsigned)MQTTTransport::reconnectCount(),
            (unsigned)TelemetryBuffer::count(),
            (unsigned)MQTTTransport::txQueueDepth(),
            (unsigned long)TimeSync::bestEffort());
        MQTTTransport::publish(MQTTTopics::diagnostics(), buf, 0, false);
    }

private:
    static bool _pendingCrash;
};
inline bool DiagnosticsManager::_pendingCrash = false;
