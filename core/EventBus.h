#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include "Types.h"

// ── Network State Bits (EventGroup) ──────────────────────────
#define NET_WIFI_CONNECTED_BIT   (1 << 0)
#define NET_MQTT_CONNECTED_BIT   (1 << 1)
#define NET_TIME_SYNCED_BIT      (1 << 2)
#define NET_OTA_ACTIVE_BIT       (1 << 3)

class EventBus {
public:
    static void init() {
        // OPP-3: _sensorQ removed — all sensor producers use BatchPublisher::stage() directly.
        _commandQ   = xQueueCreate(16, sizeof(Event));
        _alertQ     = xQueueCreate(16, sizeof(Event));
        _otaQ       = xQueueCreate(4,  sizeof(Event));
        _netStateEG = xEventGroupCreate();
        configASSERT(_commandQ && _alertQ && _otaQ && _netStateEG);
    }

    // ── Publish ───────────────────────────────────────────────
    // Post an event to the appropriate internal queue (ISR-safe variants available)

    // OPP-3: postSensor() removed — use BatchPublisher::stage() directly.

    static bool postCommand(const Event& e, TickType_t wait = pdMS_TO_TICKS(50)) {
        return xQueueSend(_commandQ, &e, wait) == pdTRUE;
    }

    static bool postAlert(const Event& e, TickType_t wait = pdMS_TO_TICKS(100)) {
        return xQueueSend(_alertQ, &e, wait) == pdTRUE;
    }

    static bool postOTA(const Event& e, TickType_t wait = 0) {
        return xQueueSend(_otaQ, &e, wait) == pdTRUE;
    }

    static bool post(const Event& e) {
        switch (e.type) {
            // ── Sensor data — use BatchPublisher::stage() directly ────
            case EventType::SENSOR_READING:
                LOG_W("EventBus", "post(SENSOR_READING) — call BatchPublisher::stage() directly");
                return false;
            // ── Network state — handled by EventGroup; no queue needed ─
            // OPP-4: these were postAlert() which wasted alert slots on events
            //         MQTTTxTask silently discards. EventGroup is the real consumer.
            case EventType::WIFI_CONNECTED:
            case EventType::WIFI_DISCONNECTED:
            case EventType::MQTT_CONN_OK:
            case EventType::MQTT_DISCONNECTED:
            case EventType::TIME_SYNCED:
            case EventType::OTA_PROGRESS:
            case EventType::CRASH_RECOVERED:
            case EventType::HEARTBEAT_TICK:
            case EventType::HEAP_LOW:
                return true;
            // ── Relay state observation ───────────────────────────
            // BUG-10 fix: RELAY_STATE_CHANGED was defined but unrouted (fell to default→drop).
            case EventType::RELAY_STATE_CHANGED:   return postAlert(e);
            // ── Actuator / system commands ────────────────────────
            case EventType::RELAY_COMMAND:
            case EventType::SERVO_COMMAND:
            case EventType::STEPPER_COMMAND:
            case EventType::COMMAND_RECEIVED:
            case EventType::CRED_ROTATE_COMMAND:
            case EventType::SYSTEM_RESTART:        return postCommand(e);
            // ── Safety alerts ─────────────────────────────────────
            case EventType::SAFETY_ALERT:          return postAlert(e);
            // ── OTA ───────────────────────────────────────────────
            case EventType::OTA_COMMAND:           return postOTA(e);
            // ── Unknown — warn and drop; do NOT pollute sensor queue
            default:
                LOG_W("EventBus", "post(): unrouted EventType %d — dropped",
                      (int)e.type);
                return false;
        }
    }

    // OPP-3: waitSensor() removed — no consumer, no queue.
    static bool waitCommand(Event& e, TickType_t t = portMAX_DELAY) {
        return xQueueReceive(_commandQ, &e, t) == pdTRUE;
    }
    static bool waitAlert  (Event& e, TickType_t t = portMAX_DELAY) {
        return xQueueReceive(_alertQ,   &e, t) == pdTRUE;
    }
    static bool waitOTA    (Event& e, TickType_t t = portMAX_DELAY) {
        return xQueueReceive(_otaQ,     &e, t) == pdTRUE;
    }

    // ── Network State EventGroup ──────────────────────────────
    static void setNetBit  (EventBits_t bits) { xEventGroupSetBits  (_netStateEG, bits); }
    static void clearNetBit(EventBits_t bits) { xEventGroupClearBits(_netStateEG, bits); }

    static bool isWifiConnected() {
        return (xEventGroupGetBits(_netStateEG) & NET_WIFI_CONNECTED_BIT) != 0;
    }
    static bool isMqttConnected() {
        return (xEventGroupGetBits(_netStateEG) & NET_MQTT_CONNECTED_BIT) != 0;
    }
    static bool isTimeSynced() {
        return (xEventGroupGetBits(_netStateEG) & NET_TIME_SYNCED_BIT) != 0;
    }
    static bool isOTAActive() {
        return (xEventGroupGetBits(_netStateEG) & NET_OTA_ACTIVE_BIT) != 0;
    }

    // Wait until WiFi+MQTT are both up (used by tasks on startup)
    static EventBits_t waitForNetwork(TickType_t timeout = portMAX_DELAY) {
        return xEventGroupWaitBits(_netStateEG,
            NET_WIFI_CONNECTED_BIT | NET_MQTT_CONNECTED_BIT,
            pdFALSE, pdTRUE, timeout);
    }

    // OPP-3: sensorQDepth() removed with _sensorQ.
    static UBaseType_t commandQDepth() { return uxQueueMessagesWaiting(_commandQ); }
    static UBaseType_t alertQDepth()   { return uxQueueMessagesWaiting(_alertQ);   }

private:
    static QueueHandle_t      _commandQ;
    static QueueHandle_t      _alertQ;
    static QueueHandle_t      _otaQ;
    static EventGroupHandle_t _netStateEG;
};

inline QueueHandle_t      EventBus::_commandQ   = nullptr;
inline QueueHandle_t      EventBus::_alertQ     = nullptr;
inline QueueHandle_t      EventBus::_otaQ       = nullptr;
inline EventGroupHandle_t EventBus::_netStateEG = nullptr;
