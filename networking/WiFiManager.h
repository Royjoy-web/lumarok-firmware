#pragma once
#include <WiFi.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../security/CredentialStore.h"
#include "../telemetry/TimeSync.h"

enum class WiFiState : uint8_t {
    DISCONNECTED, CONNECTING, CONNECTED, FAILED
};

class WiFiManager {
public:
    static void init() {
        WiFi.mode(WIFI_STA);
        WiFi.setAutoReconnect(false); // We manage reconnect ourselves
        WiFi.onEvent(_wifiEventHandler);
    }

    // Non-blocking: call repeatedly from NetworkTask
    // Returns true when connected
    static bool tick() {
        switch (_state) {
            case WiFiState::CONNECTED:
                // Detect silent drops (NAT timeout, AP channel change)
                if (WiFi.status() != WL_CONNECTED) {
                    LOG_W("WiFi", "Silent disconnect detected");
                    _onDisconnected();
                }
                return true;

            case WiFiState::DISCONNECTED:
                if (_retryCount == 0 || _backoffExpired()) {
                    _beginConnect();
                }
                return false;

            case WiFiState::CONNECTING:
                if (WiFi.status() == WL_CONNECTED) {
                    _onConnected();
                    return true;
                }
                if (millis() - _connectStartMs > WIFI_TIMEOUT_MS) {
                    _retryCount++;
                    LOG_W("WiFi", "Timeout — attempt %d", (int)_retryCount);
                    WiFi.disconnect(true);
                    _state = WiFiState::DISCONNECTED;
                    _backoffStartMs = millis();
                }
                return false;

            case WiFiState::FAILED:
                // Retry every 60s even in failed state
                if (millis() - _backoffStartMs > 60000UL) {
                    _retryCount = 0;
                    _state = WiFiState::DISCONNECTED;
                }
                return false;
        }
        return false;
    }

    static bool      isConnected() { return _state == WiFiState::CONNECTED; }
    static WiFiState state()       { return _state; }
    static int       rssi()        { return WiFi.RSSI(); }
    static String    ip()          { return WiFi.localIP().toString(); }
    static String    ssid()        { return WiFi.SSID(); }
    static uint8_t   retryCount()  { return _retryCount; }

    static void forceReconnect() {
        WiFi.disconnect(true);
        _retryCount     = 0;
        _backoffStartMs = 0;
        _state          = WiFiState::DISCONNECTED;
    }

private:
    static void _beginConnect() {
        String ssid = CredentialStore::wifiSSID();
        String pass = CredentialStore::wifiPass();
        if (ssid.isEmpty()) {
            LOG_E("WiFi", "No SSID stored");
            _state = WiFiState::FAILED;
            return;
        }
        LOG_I("WiFi", "Connecting to \"%s\" (attempt %d)", ssid.c_str(), (int)_retryCount + 1);
        WiFi.begin(ssid.c_str(), pass.c_str());
        _connectStartMs = millis();
        _state          = WiFiState::CONNECTING;
    }

    static void _onConnected() {
        _state      = WiFiState::CONNECTED;
        _retryCount = 0;

        // Publish WIFI_CONNECTED event
        Event e{}; e.type = EventType::WIFI_CONNECTED;
        strncpy(e.data.net.ip,   WiFi.localIP().toString().c_str(), 15);
        strncpy(e.data.net.ssid, WiFi.SSID().c_str(), 31);
        e.data.net.rssi = WiFi.RSSI();
        EventBus::setNetBit(NET_WIFI_CONNECTED_BIT);
        // OPP-3: postSensor(WIFI_CONNECTED) removed — EventGroup bit is the consumer.

        // Start SNTP now that we have connectivity
        TimeSync::init();

        LOG_I("WiFi", "Connected — IP: %s  RSSI: %d dBm",
              WiFi.localIP().toString().c_str(), WiFi.RSSI());

        // Increment reconnect counter in diagnostics store
        uint32_t cnt = NVSStore::getU32(NVS_NS_DIAG, "wifi_rc", 0);
        NVSStore::putU32(NVS_NS_DIAG, "wifi_rc", cnt + 1);
    }

    static void _onDisconnected() {
        bool wasConnected = (_state == WiFiState::CONNECTED);
        _state          = WiFiState::DISCONNECTED;
        _backoffStartMs = millis();
        EventBus::clearNetBit(NET_WIFI_CONNECTED_BIT);
        EventBus::clearNetBit(NET_MQTT_CONNECTED_BIT);  // MQTT implied disconnected too

        if (wasConnected) {
            // OPP-3: postSensor(WIFI_DISCONNECTED) removed — clearNetBit above is the signal.
            LOG_W("WiFi", "Disconnected");
        }
    }

    static void _wifiEventHandler(WiFiEvent_t event, WiFiEventInfo_t info) {
        if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
            if (_state == WiFiState::CONNECTED) {
                _onDisconnected();
            }
        }
    }

    // Exponential backoff: 2s, 4s, 8s … cap at 120s + ±25% jitter
    static bool _backoffExpired() {
        if (_backoffStartMs == 0) return true;
        uint32_t base  = min((uint32_t)(MQTT_RECONNECT_BASE_MS << min(_retryCount, (uint8_t)6)),
                              (uint32_t)MQTT_RECONNECT_MAX_MS);
        uint32_t jitter = (base / 4) * (random(0, 2) ? 1 : -1);
        return (millis() - _backoffStartMs) >= (base + jitter);
    }

    static WiFiState    _state;
    static uint8_t      _retryCount;
    static unsigned long _connectStartMs;
    static unsigned long _backoffStartMs;
};

inline WiFiState     WiFiManager::_state          = WiFiState::DISCONNECTED;
inline uint8_t       WiFiManager::_retryCount     = 0;
inline unsigned long WiFiManager::_connectStartMs = 0;
inline unsigned long WiFiManager::_backoffStartMs = 0;
