#pragma once
#include <Arduino.h>
#include "../core/Identity.h"

// ── Topic Builders ────────────────────────────────────────────
// All topic strings are pre-built into fixed char buffers at boot.
// No dynamic String allocation in publish hot-paths.

class MQTTTopics {
public:
    // Call once after Identity::init()
    static void init(const String& unitId) {
        snprintf(_deviceState,  sizeof(_deviceState),
                 "lumarok/%s/+/+/state",   unitId.c_str());
        snprintf(_sensorData,   sizeof(_sensorData),
                 "lumarok/%s/sensors/data", unitId.c_str());
        snprintf(_alerts,       sizeof(_alerts),
                 "lumarok/%s/alerts",       unitId.c_str());
        snprintf(_sysStatus,    sizeof(_sysStatus),
                 "lumarok/%s/system/status", unitId.c_str());
        snprintf(_sysOnline,    sizeof(_sysOnline),
                 "lumarok/%s/system/online", unitId.c_str());
        snprintf(_sysCmd,       sizeof(_sysCmd),
                 "lumarok/%s/system/command", unitId.c_str());
        snprintf(_otaUpdate,    sizeof(_otaUpdate),
                 "lumarok/%s/ota/update",   unitId.c_str());
        snprintf(_otaProgress,  sizeof(_otaProgress),
                 "lumarok/%s/ota/progress", unitId.c_str());
        snprintf(_cmdSub,       sizeof(_cmdSub),
                 "lumarok/%s/+/+/command",  unitId.c_str());
        snprintf(_diagnostics,  sizeof(_diagnostics),
                 "lumarok/%s/system/diagnostics", unitId.c_str());
        snprintf(_relayState,   sizeof(_relayState),
                 "lumarok/%s/relay/state",         unitId.c_str());
        _unitId = unitId;
    }

    // Pre-built publish topics (const char* — no heap allocation)
    static const char* sensorData()  { return _sensorData;  }
    static const char* alerts()      { return _alerts;       }
    static const char* sysStatus()   { return _sysStatus;    }
    // onlineTopic() is separate from lwtTopic() — the LWT broker payload
    // (offline) and the connect-time online publish must not share the same
    // retained topic or the online message overwrites the LWT registration.
    static const char* onlineTopic() { return _sysOnline;    }
    static const char* otaProgress() { return _otaProgress;  }
    static const char* diagnostics() { return _diagnostics;  }
    static const char* relayState()  { return _relayState;   }

    // Subscribe wildcards
    static const char* cmdSubscribe()  { return _cmdSub;    }
    static const char* otaSubscribe()  { return _otaUpdate; }
    static const char* sysSubscribe()  { return _sysCmd;    }

    // LWT topic + payload (MQTT client registers at connect time)
    static const char* lwtTopic()   { return _sysStatus; }
    static const char* lwtPayload() { return "{\"online\":false}"; }

    // Dynamic builders — write into provided buffer, no heap
    static void deviceStateTopic(char* buf, size_t len,
                                  const char* room, const char* device) {
        snprintf(buf, len, "lumarok/%s/%s/%s/state",
                 _unitId.c_str(), room, device);
    }

    static void deviceCmdTopic(char* buf, size_t len,
                                const char* room, const char* device) {
        snprintf(buf, len, "lumarok/%s/%s/%s/command",
                 _unitId.c_str(), room, device);
    }

    // Parse incoming command topic → extract room and device name
    // Topic form: "lumarok/{unit_id}/{room}/{device}/command"
    static bool parseCommandTopic(const char* topic,
                                   char* roomOut,  size_t roomLen,
                                   char* devOut,   size_t devLen) {
        // Find 3rd slash (after unit_id)
        const char* p = topic;
        int slashes = 0;
        while (*p && slashes < 2) { if (*p++ == '/') slashes++; }
        if (!*p) return false;
        const char* roomStart = p;

        // Room = next segment
        const char* slash3 = strchr(p, '/');
        if (!slash3) return false;
        size_t rLen = min((size_t)(slash3 - roomStart), roomLen - 1);
        strncpy(roomOut, roomStart, rLen);
        roomOut[rLen] = '\0';

        // Device = segment after room
        const char* devStart = slash3 + 1;
        const char* slash4   = strchr(devStart, '/');
        if (!slash4) return false;
        size_t dLen = min((size_t)(slash4 - devStart), devLen - 1);
        strncpy(devOut, devStart, dLen);
        devOut[dLen] = '\0';
        return true;
    }

private:
    static char   _deviceState[80];
    static char   _sensorData[80];
    static char   _relayState[80];
    static char   _alerts[80];
    static char   _sysStatus[80];
    static char   _sysOnline[80];
    static char   _sysCmd[80];
    static char   _otaUpdate[80];
    static char   _otaProgress[80];
    static char   _cmdSub[80];
    static char   _diagnostics[80];
    static String _unitId;
};

inline char   MQTTTopics::_deviceState[80]  = {};
inline char   MQTTTopics::_sensorData[80]   = {};
inline char   MQTTTopics::_alerts[80]       = {};
inline char   MQTTTopics::_sysStatus[80]    = {};
inline char   MQTTTopics::_sysOnline[80]    = {};
inline char   MQTTTopics::_sysCmd[80]       = {};
inline char   MQTTTopics::_otaUpdate[80]    = {};
inline char   MQTTTopics::_otaProgress[80]  = {};
inline char   MQTTTopics::_cmdSub[80]       = {};
inline char   MQTTTopics::_diagnostics[80]  = {};
inline char   MQTTTopics::_relayState[80]   = {};
inline String MQTTTopics::_unitId;
