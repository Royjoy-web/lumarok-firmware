#pragma once
#include <ArduinoJson.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/Identity.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../storage/TelemetryBuffer.h"
#include "../telemetry/TimeSync.h"

class TelemetryPipeline {
public:
    // ── Single reading → MQTT or buffer ──────────────────────
    static void publish(const SensorReading& r) {
        if (!EventBus::isMqttConnected()) {
            TelemetryBuffer::push(r);
            return;
        }
        char payload[512];
        _buildPayload(r, payload, sizeof(payload));
        MQTTTransport::publish(MQTTTopics::sensorData(), payload, 0, false);
        _seq++;
    }

    // ── Batch: drain offline buffer at rate-limited pace ─────
    // Call from DiagnosticsTask / TxTask after MQTT reconnect
    static void drainBuffer(uint8_t maxPerCall = 5) {
        if (!EventBus::isMqttConnected()) return;
        uint8_t sent = 0;
        SensorReading r;
        while (sent < maxPerCall && TelemetryBuffer::peek(r)) {
            char payload[512];
            _buildPayload(r, payload, sizeof(payload));
            MQTTTransport::publish(MQTTTopics::sensorData(), payload, 0, false);
            TelemetryBuffer::pop();
            sent++;
            vTaskDelay(pdMS_TO_TICKS(200)); // 5 msg/sec rate limit
        }
        if (sent > 0) LOG_I("Telem", "Drained %d buffered readings", (int)sent);
    }

    // ── Alert publish — QoS 1 ─────────────────────────────────
    static void publishAlert(const Alert& a) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "{\"type\":\"%s\",\"message\":\"%s\",\"device\":\"%s\","
                 "\"severity\":%d,\"ts\":%lu,\"unit_id\":\"%s\"}",
                 a.type, a.message, a.device, (int)a.severity,
                 (unsigned long)a.timestamp, Identity::get().c_str());
        MQTTTransport::publish(MQTTTopics::alerts(), buf, 1, false);
    }

    // ── Device state publish ──────────────────────────────────
    static void publishDeviceState(const DeviceState& d) {
        char topic[96], payload[200];
        MQTTTopics::deviceStateTopic(topic, sizeof(topic), d.room, d.name);
        snprintf(payload, sizeof(payload),
                 "{\"device_id\":\"%s\",\"state\":\"%s\",\"power\":%s,"
                 "\"ts\":%lu,\"unit_id\":\"%s\"}",
                 d.device_id,
                 d.power_state ? "on" : "off",
                 d.power_state ? "true" : "false",
                 (unsigned long)TimeSync::bestEffort(),
                 Identity::get().c_str());
        MQTTTransport::publish(topic, payload, 0, true); // retain state
    }

    // ── Heartbeat / system status ─────────────────────────────
    static void publishHeartbeat() {
        char buf[512];
        snprintf(buf, sizeof(buf),
                 "{\"online\":true,\"fw\":\"%s\",\"unit_id\":\"%s\","
                 "\"uptime\":%lu,\"rssi\":%d,\"heap\":%u,\"ts\":%lu}",
                 FIRMWARE_VERSION, Identity::get().c_str(),
                 millis() / 1000UL,
                 WiFi.RSSI(),
                 ESP.getFreeHeap(),
                 (unsigned long)TimeSync::bestEffort());
        MQTTTransport::publish(MQTTTopics::sysStatus(), buf, 1, true);
    }

    static uint32_t sequence() { return _seq; }

private:
    static void _buildPayload(const SensorReading& r, char* buf, size_t len) {
        snprintf(buf, len,
                 "{\"sensor_id\":\"%s\",\"room\":\"%s\",\"name\":\"%s\","
                 "\"value\":%.2f,\"unit\":\"%s\",\"ts\":%lu,"
                 "\"ts_uncertain\":%s,\"seq\":%u,\"quality\":\"%s\","
                 "\"errors\":%u,\"unit_id\":\"%s\"}",
                 r.sensor_id, r.room, r.name,
                 r.value, r.unit,
                 (unsigned long)r.timestamp,
                 r.ts_uncertain ? "true" : "false",
                 (unsigned int)r.seq,
                 r.quality_ok   ? "good" : "degraded",
                 (unsigned int)r.error_count,
                 Identity::get().c_str());
    }

    static uint32_t _seq;
};

inline uint32_t TelemetryPipeline::_seq = 0;
