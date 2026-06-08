#pragma once
#include <WiFi.h>
#include "../storage/NVSStore.h"
#include "../core/Config.h"
#include "../core/Types.h"

class Identity {
public:
    // Called once in setup(), before WiFi.begin()
    static void init() {
        WiFi.mode(WIFI_STA);   // needed to read MAC before connect
        _unitId = NVSStore::getString(NVS_NS_IDENTITY, NVS_KEY_UNIT_ID);
        if (_unitId.length() > 0) {
            LOG_I("ID", "Unit ID (NVS): %s", _unitId.c_str());
            return;
        }
        // Generate from full MAC — no truncation to avoid collisions
        String mac = WiFi.macAddress();
        mac.replace(":", "");
        _unitId = "LMR-" + mac;
        NVSStore::putString(NVS_NS_IDENTITY, NVS_KEY_UNIT_ID, _unitId);
        LOG_I("ID", "Unit ID (new): %s", _unitId.c_str());
    }

    static const String& get() { return _unitId; }

    // Returns the MQTT client ID derived from unit ID
    static String mqttClientId() { return "lmr-" + _unitId; }

private:
    static String _unitId;
};

// Defined in firmware.ino or Identity.cpp
// Declared here to allow header-only usage in PlatformIO
inline String Identity::_unitId;
