#pragma once
// Serial provisioning is compiled out in production builds.
// Enable only in factory flash firmware via:
//   build_flags = -D ENABLE_SERIAL_PROV=true
#if ENABLE_SERIAL_PROV

#include <ArduinoJson.h>
#include "../core/Config.h"
#include "../provisioning/ProvisioningManager.h"

class SerialProvisioning {
public:
    static void start() {
        _startMs = millis();
        _active  = true;
        _success = false;
        Serial.println(F("[PROV] PROV_READY"));
        Serial.printf("[PROV] Serial provisioning window: %lu ms\n",
                       (unsigned long)SERIAL_PROV_TIMEOUT_MS);
        ProvisioningManager::setActiveMode(ProvMode::SERIAL_PROV);
    }

    // Returns true when window closes or provisioning completes
    static bool tick() {
        if (!_active) return true;

        if (millis() - _startMs > SERIAL_PROV_TIMEOUT_MS) {
            LOG_I("SerProv", "Serial provisioning window closed");
            _active = false;
            ProvisioningManager::setActiveMode(ProvMode::NONE);
            return true;
        }

        while (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                if (_bufIdx > 0) {
                    _buf[_bufIdx] = '\0';
                    _handleLine(String(_buf));
                    _bufIdx = 0;
                }
            } else if (_bufIdx < (int)sizeof(_buf) - 1) {
                _buf[_bufIdx++] = c;
            }
        }
        return _success;
    }

    static bool isActive()  { return _active; }
    static bool isSuccess() { return _success; }

private:
    static void _handleLine(const String& line) {
        if (!line.startsWith("{")) return;

        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, line) != DeserializationError::Ok) {
            Serial.println(F("[PROV] ERROR:JSON_PARSE"));
            return;
        }

        bool ok = ProvisioningManager::commitCredentials(
            doc["wifi_ssid"]   | "",
            doc["wifi_pass"]   | "",
            doc["mqtt_user"]   | "",
            doc["mqtt_pass"]   | "",
            doc["dev_secret"]  | "",
            doc["backend_url"] | ""
        );

        Serial.println(ok ? F("[PROV] OK:PROVISIONED") : F("[PROV] ERROR:COMMIT_FAILED"));
        if (ok) {
            _success = true;
            _active  = false;
        }
    }

    static bool          _active;
    static bool          _success;
    static unsigned long _startMs;
    static char          _buf[640];
    static int           _bufIdx;
};

inline bool          SerialProvisioning::_active  = false;
inline bool          SerialProvisioning::_success = false;
inline unsigned long SerialProvisioning::_startMs = 0;
inline char          SerialProvisioning::_buf[640] = {};
inline int           SerialProvisioning::_bufIdx  = 0;

#else
class SerialProvisioning {
public:
    static void start()           {}
    static bool tick()            { return true; }
    static bool isActive()        { return false; }
    static bool isSuccess()       { return false; }
};
#endif
