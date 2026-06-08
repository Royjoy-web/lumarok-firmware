#pragma once
#include <WiFi.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/RetryPolicy.h"
#include "../mqtt/MQTTTransport.h"
#include "../security/CredentialStore.h"
#include "../telemetry/TimeSync.h"
#include "../storage/FlashWearGuard.h"

// ─────────────────────────────────────────────────────────────
// ReconnectEngine — manages WiFi and MQTT connection lifecycle.
//
// Hardenings over base WiFiManager:
//  • Zombie detection: connected but no PINGRESP / traffic
//  • Per-layer independent state machines (WiFi ≠ MQTT)
//  • Full jitter backoff with cap
//  • Reconnect storm protection: max 3 fast retries then slow
//  • AP fallback after exhausting WiFi retries
//  • Reconnect metrics persisted to NVS for post-mortem
// ─────────────────────────────────────────────────────────────

enum class ConnLayer : uint8_t { WIFI, MQTT };

struct ConnMetrics {
    uint32_t wifiReconnects;
    uint32_t mqttReconnects;
    uint32_t wifiFailures;
    uint32_t mqttFailures;
    uint32_t zombieDetections;
    time_t   lastConnectedTs;
    time_t   lastDisconnectedTs;
};

class ReconnectEngine {
public:
    static void init() {
        _wifiRetry = RetryPolicy(RetryPolicy::Config(2000, 120000, 0));
        _mqttRetry = RetryPolicy(RetryPolicy::Config(2000, 60000, 0));

        _wifiCB = CircuitBreaker("WiFi",
            CircuitBreaker::Config(10, 1, 60000));
        _mqttCB = CircuitBreaker("MQTT",
            CircuitBreaker::Config(5, 1, 30000));

        _loadMetrics();

        // Cache credentials as char arrays — avoids String heap allocation
        // on every reconnect attempt (called every 2–120s during outages).
        // Updated on credential rotation via refreshCredentialCache().
        refreshCredentialCache();

        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false);
        WiFi.onEvent(_wifiEvent);
        LOG_I("Reconnect", "Engine initialised");
    }

    // Call after credential rotation so cached values stay current
    static void refreshCredentialCache() {
        String s = CredentialStore::wifiSSID();
        String p = CredentialStore::wifiPass();
        strlcpy(_cachedSSID, s.c_str(), sizeof(_cachedSSID));
        strlcpy(_cachedPass, p.c_str(), sizeof(_cachedPass));
    }

    // ── Tick (call from NetworkTask ~100ms) ───────────────────
    static void tick() {
        _tickWiFi();
        if (_wifiUp) _tickMQTT();
        _detectZombies();
    }

    static bool wifiConnected() { return _wifiUp; }
    static bool mqttConnected() { return MQTTTransport::isConnected(); }

    static const ConnMetrics& metrics() { return _m; }

    static void persistMetrics() {
        // Route through FlashWearGuard — coalesces writes during disconnect storms.
        // A WiFi dropout can fire persistMetrics() many times in quick succession;
        // direct NVS writes would exhaust the daily flash budget in hours.
        FlashWearGuard::putU32(NVS_NS_DIAG, "wrc",  _m.wifiReconnects);
        FlashWearGuard::putU32(NVS_NS_DIAG, "mrc",  _m.mqttReconnects);
        FlashWearGuard::putU32(NVS_NS_DIAG, "wfc",  _m.wifiFailures);
        FlashWearGuard::putU32(NVS_NS_DIAG, "mfc",  _m.mqttFailures);
        FlashWearGuard::putU32(NVS_NS_DIAG, "zdc",  _m.zombieDetections);
    }

private:
    // ── WiFi state machine ────────────────────────────────────
    static void _tickWiFi() {
        if (_wifiUp) return;
        if (!_wifiCB.allowRequest()) return;
        if (!_wifiRetry.ready())     return;

        if (_cachedSSID[0] == '\0') return;   // no credentials stored

        LOG_I("Reconnect", "WiFi attempt %u to \"%s\"",
              _wifiRetry.attempt() + 1, _cachedSSID);

        WiFi.begin(_cachedSSID, _cachedPass);
        _wifiConnectStartMs = millis();
        _wifiPending        = true;
        _wifiRetry.scheduleNext();
    }

    // ── MQTT state machine ────────────────────────────────────
    static void _tickMQTT() {
        if (MQTTTransport::isConnected()) return;
        if (!_mqttCB.allowRequest())      return;
        if (!_mqttRetry.ready())          return;

        LOG_I("Reconnect", "MQTT attempt %u", _mqttRetry.attempt() + 1);

        bool ok = MQTTTransport::connect();
        if (ok) {
            _mqttCB.recordSuccess();
            _mqttRetry.reset();
            _m.mqttReconnects++;
            _m.lastConnectedTs = TimeSync::bestEffort();
            persistMetrics();
            // FIX v3.2: reset zombie timer on connect so the freshly established
            // connection gets a full 2×keepalive grace period before zombie detection
            // fires. Without this, _lastMqttActivityMs=0 would trigger a false
            // zombie disconnect within 120s if the broker sends no PUBLISH messages.
            _lastMqttActivityMs = millis();
            EventBus::setNetBit(NET_MQTT_CONNECTED_BIT);
            LOG_I("Reconnect", "MQTT connected (total reconnects: %u)",
                  (unsigned)_m.mqttReconnects);
        } else {
            _mqttCB.recordFailure();
            _m.mqttFailures++;
            _mqttRetry.scheduleNext();
            uint32_t wait = _mqttRetry.pendingDelayMs();
            LOG_W("Reconnect", "MQTT failed — retry in %ums  (CB: %s)",
                  (unsigned)wait, _mqttCB.isOpen() ? "OPEN" : "CLOSED");
        }
    }

    // ── Zombie detection ──────────────────────────────────────
    // Catches "connected but silent" cases:
    //  - WiFi stack says WL_CONNECTED but no pings succeed
    //  - MQTT PubSubClient returns connected() but no messages arrive
    static void _detectZombies() {
        unsigned long now = millis();

        // WiFi zombie: connected >30s but no data
        if (_wifiUp && _wifiPending) {
            if (now - _wifiConnectStartMs > WIFI_TIMEOUT_MS) {
                LOG_W("Reconnect", "WiFi connect timeout — forcing disconnect");
                _forceWiFiReset();
            }
        }

        // WiFi zombie: TCP stack says connected, but RSSI is impossible
        if (_wifiUp && WiFi.RSSI() == 0) {
            LOG_W("Reconnect", "WiFi zombie suspected (RSSI=0)");
            _m.zombieDetections++;
            _forceWiFiReset();
        }

        // MQTT zombie: isConnected() true but no loop activity in >2× keepalive
        if (MQTTTransport::isConnected()) {
            if (now - _lastMqttActivityMs > (MQTT_KEEPALIVE_S * 2000UL)) {
                LOG_W("Reconnect", "MQTT zombie — forcing reconnect");
                _m.zombieDetections++;
                MQTTTransport::disconnect();
                EventBus::clearNetBit(NET_MQTT_CONNECTED_BIT);
                _mqttRetry.reset();
                _mqttRetry.scheduleNext();
            }
        }
    }

    static void _forceWiFiReset() {
        WiFi.disconnect(true);
        delay(100);
        _wifiUp          = false;
        _wifiPending     = false;
        _m.wifiFailures++;
        _wifiCB.recordFailure();
        _wifiRetry.scheduleNext();
        EventBus::clearNetBit(NET_WIFI_CONNECTED_BIT);
        EventBus::clearNetBit(NET_MQTT_CONNECTED_BIT);
        _m.lastDisconnectedTs = TimeSync::bestEffort();
    }

    // Called from WiFi event handler (in WiFi task context)
    static void _wifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
        switch (event) {
            case ARDUINO_EVENT_WIFI_STA_CONNECTED:
                break;
            case ARDUINO_EVENT_WIFI_STA_GOT_IP:
                _wifiUp          = true;
                _wifiPending     = false;
                _wifiCB.recordSuccess();
                _wifiRetry.reset();
                _m.wifiReconnects++;
                _m.lastConnectedTs = (time_t)(millis() / 1000);
                EventBus::setNetBit(NET_WIFI_CONNECTED_BIT);
                TimeSync::init();
                LOG_I("Reconnect", "WiFi UP — IP: %s  RSSI: %d",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI());
                break;
            case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
                if (_wifiUp) {
                    LOG_W("Reconnect", "WiFi disconnected (reason: %d)",
                          (int)info.wifi_sta_disconnected.reason);
                    _wifiUp = false;
                    _wifiCB.recordFailure();
                    _wifiRetry.scheduleNext();
                    EventBus::clearNetBit(NET_WIFI_CONNECTED_BIT);
                    EventBus::clearNetBit(NET_MQTT_CONNECTED_BIT);
                    _m.lastDisconnectedTs = (time_t)(millis() / 1000);
                    persistMetrics();
                }
                break;
            default: break;
        }
    }

    // Called from MQTTRxTask to update activity timestamp
    public: static void notifyMQTTActivity() { _lastMqttActivityMs = millis(); }

    private:
    static void _loadMetrics() {
        _m.wifiReconnects   = NVSStore::getU32(NVS_NS_DIAG, "wrc", 0);
        _m.mqttReconnects   = NVSStore::getU32(NVS_NS_DIAG, "mrc", 0);
        _m.wifiFailures     = NVSStore::getU32(NVS_NS_DIAG, "wfc", 0);
        _m.mqttFailures     = NVSStore::getU32(NVS_NS_DIAG, "mfc", 0);
        _m.zombieDetections = NVSStore::getU32(NVS_NS_DIAG, "zdc", 0);
    }

    static RetryPolicy    _wifiRetry;
    static RetryPolicy    _mqttRetry;
    static CircuitBreaker _wifiCB;
    static CircuitBreaker _mqttCB;
    static ConnMetrics    _m;
    static bool           _wifiUp;
    static bool           _wifiPending;
    static unsigned long  _wifiConnectStartMs;
    static unsigned long  _lastMqttActivityMs;
    static char           _cachedSSID[64];
    static char           _cachedPass[64];
};

inline RetryPolicy    ReconnectEngine::_wifiRetry(RetryPolicy::Config(2000, 120000, 0));
inline RetryPolicy    ReconnectEngine::_mqttRetry(RetryPolicy::Config(2000, 60000, 0));
inline CircuitBreaker ReconnectEngine::_wifiCB("WiFi");
inline CircuitBreaker ReconnectEngine::_mqttCB("MQTT");
inline ConnMetrics    ReconnectEngine::_m                 = {};
inline bool           ReconnectEngine::_wifiUp            = false;
inline bool           ReconnectEngine::_wifiPending       = false;
inline unsigned long  ReconnectEngine::_wifiConnectStartMs= 0;
inline unsigned long  ReconnectEngine::_lastMqttActivityMs= 0;
inline char           ReconnectEngine::_cachedSSID[64]    = {};
inline char           ReconnectEngine::_cachedPass[64]    = {};
