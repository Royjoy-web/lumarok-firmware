#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../core/BootManager.h"
#include "../diagnostics/DiagnosticsManager.h"
#include "../diagnostics/WatchdogManager.h"
#include "../diagnostics/FaultManager.h"
#include "../storage/PriorityBuffer.h"
#include "../diagnostics/HeapMonitor.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../telemetry/BatchPublisher.h"
#include "../storage/FlashWearGuard.h"
#include "../networking/ReconnectEngine.h"
#include "../mqtt/MQTTTopics.h"
#include "../mqtt/MQTTTransport.h"
#include "../core/Identity.h"
#include "../core/Config.h"

static void _publishFullDiagnostics();
static uint16_t _crc16(const uint8_t* d, size_t n);  // defined after _publishFullDiagnostics()
void diagnosticsTaskFnV2(void* pvParam) {
    // DiagnosticsTask does NOT register with hardware WDT —
    // it is the lowest priority and must not block the WDT scan.
    // It uses its own software heartbeat only.
    DiagnosticsManager::init();

    unsigned long lastDiagMs    = 0;
    unsigned long lastWDMScanMs = 0;
    unsigned long lastDrainMs   = 0;
    unsigned long lastFaultMs   = 0;
    bool          stableConfirmed = false;   // BUG-E fix: track one-shot stable confirmation

    static constexpr uint32_t DIAG_MS      = 60000UL;
    static constexpr uint32_t WDM_SCAN_MS  = 1000UL;
    static constexpr uint32_t DRAIN_MS     = 15000UL;
    static constexpr uint32_t FAULT_PUB_MS = 120000UL;

    LOG_I("DiagV2", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        unsigned long now = millis();

        // ── Heap monitor — every tick ──────────────────────────
        HeapMonitor::tick();

        // ── Heap threshold enforcement (soak-test calibrated) ────
        // UPGRADE v3.2: Feed FaultManager::HEAP_LOW based on soak thresholds
        // defined in Config.h so field devices raise alerts automatically
        // rather than requiring manual inspection of diagnostics JSON.
        {
            uint32_t freeHeap = HeapMonitor::free();
            if (freeHeap > 0) {
                if (freeHeap < HEAP_FREE_CRIT_BYTES) {
                    FaultManager::record(FaultCode::HEAP_LOW, "crit",
                                         FaultSeverity::SEV_CRITICAL);
                } else if (freeHeap < HEAP_FREE_WARN_BYTES) {
                    FaultManager::record(FaultCode::HEAP_LOW, "warn",
                                         FaultSeverity::SEV_MEDIUM);
                }
                // Clear is now conditioned on both heap quantity AND fragmentation — see below.
                // Fragmentation check (largest_block / free = contiguity ratio)
                uint32_t largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
                uint8_t  fragPct = (uint8_t)((largest * 100UL) / freeHeap);
                if (fragPct < HEAP_FRAG_CRIT_PCT) {
                    // At <40% contiguity large JSON publishes will fail silently
                    FaultManager::record(FaultCode::HEAP_LOW, "frag_crit",
                                         FaultSeverity::SEV_HIGH);
                } else if (freeHeap >= HEAP_FREE_WARN_BYTES) {
                    // BUG-8 fix: clear frag fault only when BOTH heap quantity AND
                    // fragmentation are healthy — prevents quantity-clear wiping a
                    // live frag fault, and adds an explicit recovery path for frag.
                    FaultManager::clear(FaultCode::HEAP_LOW);
                }
            }
        }

        // ── Per-task watchdog scan — every 1s ─────────────────
        if (now - lastWDMScanMs >= WDM_SCAN_MS) {
            lastWDMScanMs = now;
            WatchdogManager::scan();
        }

        // ── BUG-E fix: confirm stable operation once after DEEP_IDLE_THRESHOLD_MS
        // BootManager::confirmStableOperation() was defined but never called,
        // so the crash counter never decremented after recovery. A device with
        // 3+ prior panics would rollback on its next unrelated crash even after
        // running cleanly for days. Now: after 5 min uptime with no active
        // faults, clear the counter exactly once per boot.
        if (!stableConfirmed && now >= DEEP_IDLE_THRESHOLD_MS &&
            !FaultManager::isActive(FaultCode::HEAP_LOW) &&
            FaultManager::mode() == SystemMode::NORMAL) {
            stableConfirmed = true;
            BootManager::confirmStableOperation();
        }

        if (!EventBus::isMqttConnected()) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // ── Full diagnostics — every 60s ──────────────────────
        if (now - lastDiagMs >= DIAG_MS) {
            lastDiagMs = now;
            _publishFullDiagnostics();
        }

        // ── Fault report — every 2 min if any active ──────────
        if (now - lastFaultMs >= FAULT_PUB_MS) {
            lastFaultMs = now;
            char buf[384];
            FaultManager::buildFaultJSON(buf, sizeof(buf));
            if (strstr(buf, "\"active\":true")) {
                MQTTTransport::publish(MQTTTopics::diagnostics(), buf, 0, false);
            }
        }

        // ── Drain offline buffer — every 15s ─────────────────
        if (now - lastDrainMs >= DRAIN_MS) {
            lastDrainMs = now;
            BatchPublisher::drainOffline(3);
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void _publishFullDiagnostics() {
    char taskBuf[320];
    WatchdogManager::buildHealthJSON(taskBuf, sizeof(taskBuf));

    const ConnMetrics& cm = ReconnectEngine::metrics();

    // Fragmentation: 100% = perfectly contiguous, <50% = badly fragmented
    uint8_t fragPct = (HeapMonitor::free() > 0)
        ? (uint8_t)((HeapMonitor::largest() * 100UL) / HeapMonitor::free())
        : 100;

    char buf[896];
    snprintf(buf, sizeof(buf),
        "{\"unit\":\"%s\",\"fw\":\"%s\",\"uptime\":%lu,"
        "\"heap\":{\"free\":%u,\"min\":%u,\"largest\":%u,\"frag_pct\":%u},"
        "\"flash_writes\":%u,"
        "\"wifi\":{\"rssi\":%d,\"connected\":%s,\"ssid\":\"%s\"},"
        "\"mqtt\":{\"connected\":%s,\"reconnects\":%u,\"tx_queue\":%u},"
        "\"reconnects\":{\"wifi\":%u,\"mqtt\":%u,"
        "\"wifi_fail\":%u,\"mqtt_fail\":%u,\"zombies\":%u},"
        "\"buffer\":{\"high\":%u,\"normal\":%u,\"nvsq\":%u,\"dropped\":%u},"
        "\"mode\":%d,"
        "\"tasks\":%s,"
        "\"ts\":%lu}",
        Identity::get().c_str(), FIRMWARE_VERSION,
        millis() / 1000UL,
        HeapMonitor::free(), HeapMonitor::minFree(),
        (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), (unsigned)fragPct,
        (unsigned)FlashWearGuard::lifetimeWrites(),
        WiFi.RSSI(),
        ReconnectEngine::wifiConnected() ? "true" : "false",
        WiFi.SSID().c_str(),
        MQTTTransport::isConnected() ? "true" : "false",
        (unsigned)MQTTTransport::reconnectCount(),
        (unsigned)MQTTTransport::txQueueDepth(),
        (unsigned)cm.wifiReconnects, (unsigned)cm.mqttReconnects,
        (unsigned)cm.wifiFailures,   (unsigned)cm.mqttFailures,
        (unsigned)cm.zombieDetections,
        (unsigned)PriorityBuffer::highPending(),
        (unsigned)PriorityBuffer::normalPending(),
        (unsigned)PriorityBuffer::nvsPending(),
        (unsigned)PriorityBuffer::dropped(),
        (int)FaultManager::mode(),
        taskBuf,
        (unsigned long)TimeSync::bestEffort());

    // OPP-8: skip publish when payload unchanged
    static uint16_t lastDiagCRC = 0;
    uint16_t crc = _crc16((const uint8_t*)buf, strnlen(buf, sizeof(buf)));
    if (crc == lastDiagCRC) return;
    lastDiagCRC = crc;

    MQTTTransport::publish(MQTTTopics::diagnostics(), buf, 0, false);
}

static uint16_t _crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint16_t)d[i] << 8;
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}
