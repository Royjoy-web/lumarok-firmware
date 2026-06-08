#pragma once
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/Identity.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../storage/PriorityBuffer.h"
#include "../telemetry/TimeSync.h"

// ─────────────────────────────────────────────────────────────
// BatchPublisher — accumulates SensorReadings over a window
// and publishes them as a single JSON array payload.
//
// Reduces MQTT publish frequency by up to 8×.
// Prevents per-reading heap allocations in the sensor hot path.
// Static JSON document — no heap.
// ─────────────────────────────────────────────────────────────

#define BATCH_MAX_READINGS  8
#define BATCH_WINDOW_MS     8000UL      // flush after 8s or when full
#define BATCH_PAYLOAD_SIZE  1536        // fits in MQTT_BUFFER_SIZE

class BatchPublisher {
public:
    static void init() {
        _mutex     = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        _count     = 0;
        _windowMs  = millis();
        _pubCount  = 0;
        _readCount = 0;
    }

    // Stage a reading — does NOT publish immediately.
    // Priority 1 readings are flushed immediately (alerts, state changes).
    // THREAD SAFE: called from SafetyTask (pri 7) and SensorTaskV2 (pri 4).
    // _mutex serialises all access to _batch[], _count, and _windowMs.
    // _flush() snapshots the batch under the lock, then publishes outside it
    // so the slow MQTTTransport::publish() does not block other producers.
    static void stage(const SensorReading& r, uint8_t priority = 0) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _readCount++;

        if (!EventBus::isMqttConnected()) {
            xSemaphoreGive(_mutex);
            PriorityBuffer::push(r, priority);
            return;
        }

        if (priority > 0) {
            // High priority: flush current batch then send alone
            if (_count > 0) _flush();   // _flush() releases mutex internally
            else            xSemaphoreGive(_mutex);
            // Re-acquire to stage the single high-priority reading and flush it
            xSemaphoreTake(_mutex, portMAX_DELAY);
            _batch[0] = r;
            _count    = 1;
            _flush();   // releases mutex
            return;
        }

        _batch[_count++] = r;

        bool windowExpired = (millis() - _windowMs) >= BATCH_WINDOW_MS;
        if (_count >= BATCH_MAX_READINGS || windowExpired) {
            _flush();   // releases mutex
        } else {
            xSemaphoreGive(_mutex);
        }
    }

    // Force-flush partial batch (e.g. before sleep, on disconnect)
    static void flush() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_count > 0) _flush();   // _flush() releases mutex
        else            xSemaphoreGive(_mutex);
    }

    // Drain offline buffer — call after MQTT reconnect, rate-limited
    static void drainOffline(uint8_t maxPerCall = 4) {
        if (!EventBus::isMqttConnected()) return;
        uint8_t sent = 0;
        SensorReading r; uint8_t pri;
        time_t now = TimeSync::bestEffort();
        while (sent < maxPerCall && PriorityBuffer::peek(r, pri)) {
            // OPP-6: discard stale normal readings; always drain high-priority (safety)
            if (pri == 0 && now > 0 && (now - r.timestamp) > (time_t)OFFLINE_TTL_S) {
                PriorityBuffer::pop();
                LOG_D("Batch", "Discarded stale offline reading (age %lus)", (unsigned long)(now - r.timestamp));
                continue;
            }
            stage(r, pri);  // BUG-FIX: preserve priority — pri=1 (gas/door alerts) must flush immediately
            PriorityBuffer::pop();
            sent++;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (_count > 0) _flush();
        if (sent > 0)
            LOG_I("Batch", "Drained %d offline readings (remain: %u)",
                  (int)sent, (unsigned)PriorityBuffer::totalPending());
    }

    static uint32_t publishCount() { return _pubCount; }
    static uint32_t readingCount() { return _readCount; }
    static uint8_t  pendingCount() { return _count; }

private:
    // CALLER MUST HOLD _mutex before calling _flush().
    // _flush() snapshots _batch[] and _count under the lock, then releases
    // the mutex BEFORE calling MQTTTransport::publish() (which can block on
    // the TX queue). This prevents SafetyTask from stalling for the full
    // publish duration while holding the lock.
    static void _flush() {
        if (_count == 0) { xSemaphoreGive(_mutex); return; }

        // Snapshot under the lock
        SensorReading snap[BATCH_MAX_READINGS];
        uint8_t       snapCount = _count;
        memcpy(snap, _batch, snapCount * sizeof(SensorReading));
        _count    = 0;
        _windowMs = millis();
        uint32_t pubSeq = ++_pubCount;
        xSemaphoreGive(_mutex);

        // Build and publish outside the lock
        StaticJsonDocument<BATCH_PAYLOAD_SIZE> doc;
        doc["unit_id"] = Identity::get();
        doc["ts"]      = (unsigned long)TimeSync::bestEffort();
        doc["seq"]     = pubSeq;
        JsonArray arr  = doc.createNestedArray("readings");

        for (uint8_t i = 0; i < snapCount; i++) {
            const SensorReading& r = snap[i];
            JsonObject obj = arr.createNestedObject();
            obj["id"]      = r.sensor_id;
            obj["room"]    = r.room;
            obj["name"]    = r.name;
            obj["v"]       = serialized(String(r.value, 2));
            obj["u"]       = r.unit;
            if (r.ts_uncertain)   obj["tsu"] = true;
            if (!r.quality_ok)    obj["deg"] = true;
            if (r.error_count)    obj["err"] = r.error_count;
        }

        char payload[BATCH_PAYLOAD_SIZE];
        size_t n = serializeJson(doc, payload, sizeof(payload));
        if (n >= sizeof(payload) - 1) {
            LOG_W("Batch", "Payload truncated (%u readings)", (unsigned)snapCount);
        }

        MQTTTransport::publish(MQTTTopics::sensorData(), payload, 0, false);
        LOG_D("Batch", "Published %u readings (%u bytes)", (unsigned)snapCount, (unsigned)n);
    }

    static SensorReading _batch[BATCH_MAX_READINGS];
    static uint8_t       _count;
    static unsigned long _windowMs;
    static uint32_t      _pubCount;
    static uint32_t      _readCount;
    static SemaphoreHandle_t _mutex;
};

inline SensorReading BatchPublisher::_batch[BATCH_MAX_READINGS] = {};
inline uint8_t       BatchPublisher::_count    = 0;
inline unsigned long BatchPublisher::_windowMs = 0;
inline uint32_t      BatchPublisher::_pubCount = 0;
inline uint32_t      BatchPublisher::_readCount= 0;
inline SemaphoreHandle_t BatchPublisher::_mutex = nullptr;
