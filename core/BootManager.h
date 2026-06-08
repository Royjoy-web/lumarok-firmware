#pragma once
#include <esp_system.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../storage/NVSStore.h"

// ── Boot Mode ─────────────────────────────────────────────────
enum class BootMode : uint8_t {
    NORMAL,        // WiFi + MQTT + full operation
    PROVISIONING,  // No credentials stored — enter provisioning flow
    AP_FALLBACK,   // WiFi credentials present but connection failed
    SAFE,          // Crash loop detected — minimal mode, await OTA
};

class BootManager {
public:
    static void init() {
        _resetReason  = esp_reset_reason();
        _crashCount   = NVSStore::getU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0);
        _prevFirmware = NVSStore::getString(NVS_NS_CONFIG, NVS_KEY_PREV_FW);

        _determineMode();
        _handleCrashCount();

        LOG_I("Boot", "Reset reason : %d", (int)_resetReason);
        LOG_I("Boot", "Boot mode    : %d", (int)_mode);
        LOG_I("Boot", "Crash count  : %d", (int)_crashCount);
        if (_prevFirmware.length()) {
            LOG_I("Boot", "Prev firmware: %s", _prevFirmware.c_str());
        }
    }

    static BootMode   mode()         { return _mode; }
    static uint8_t    crashCount()   { return _crashCount; }

    // isCrashLoop: only triggers on SYSTEM crashes (panic, WDT).
    // BLE init failures use a separate BLE-specific counter in BLEProvisioning
    // and never feed into this check — so a broken BLE radio cannot lock the
    // device into safe mode.
    static bool       isCrashLoop()  { return _crashCount >= SYSTEM_CRASH_THRESHOLD; }

    static esp_reset_reason_t resetReason() { return _resetReason; }
    static const String& prevFirmware()     { return _prevFirmware; }

    // Called by WatchdogTask / stable-run confirmation after 5 min uptime
    static void confirmStableOperation() {
        if (_crashCount > 0) {
            _crashCount = 0;
            NVSStore::putU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0);
            LOG_I("Boot", "Stable — crash counter cleared");
        }
    }

    // Called by RollbackManager panic handler
    // NOTE: Do NOT call this for BLE provisioning failures.
    //       BLE has its own isolated counter (BLE_CRASH_NVS_KEY in BLEProvisioning.h).
    static void incrementCrashCount() {
        // Cap at 10 to limit NVS wear — safe mode triggers at threshold anyway
        if (_crashCount < 10) {
            _crashCount++;
            NVSStore::putU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, _crashCount);
        }
    }

    // Force-reset the system crash counter (e.g. after factory reset command)
    static void resetCrashCount() {
        _crashCount = 0;
        NVSStore::putU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0);
        LOG_I("Boot", "Crash counter force-reset");
    }

    // Store current FW version before an OTA-triggered restart
    static void recordFirmwareVersion(const String& ver) {
        NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_PREV_FW,     ver);
        NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_FW_VERSION,  FIRMWARE_VERSION);
    }

private:
    // Crash thresholds — defined in Config.h to share with RollbackManager.
    // Rollback fires at ROLLBACK_CRASH_THRESHOLD (3) before safe mode here (5).
    static constexpr uint8_t SYSTEM_CRASH_THRESHOLD = SAFE_MODE_CRASH_THRESHOLD;

    static void _determineMode() {
        bool hasWifi  = NVSStore::exists(NVS_NS_CONFIG, NVS_KEY_WIFI_SSID);
        bool hasMqtt  = NVSStore::exists(NVS_NS_SECRETS, NVS_KEY_MQTT_USER);

        if (!hasWifi || !hasMqtt) {
            _mode = BootMode::PROVISIONING;
            return;
        }
        if (isCrashLoop()) {
            _mode = BootMode::SAFE;
            LOG_W("Boot", "CRASH LOOP DETECTED (%d crashes) — entering safe mode",
                  (int)_crashCount);
            return;
        }
        _mode = BootMode::NORMAL;
    }

    static void _handleCrashCount() {
        esp_reset_reason_t r = _resetReason;
        bool unexpectedReset = (r == ESP_RST_PANIC    ||
                                r == ESP_RST_INT_WDT  ||
                                r == ESP_RST_TASK_WDT ||
                                r == ESP_RST_WDT);
        if (unexpectedReset) {
            incrementCrashCount();
            LOG_W("Boot", "Unexpected reset — system crash count now %d (threshold %d)",
                  (int)_crashCount, (int)SYSTEM_CRASH_THRESHOLD);
        }
    }

    static BootMode           _mode;
    static uint8_t            _crashCount;
    static String             _prevFirmware;
    static esp_reset_reason_t _resetReason;
};

inline BootMode           BootManager::_mode         = BootMode::NORMAL;
inline uint8_t            BootManager::_crashCount   = 0;
inline String             BootManager::_prevFirmware;
inline esp_reset_reason_t BootManager::_resetReason  = ESP_RST_UNKNOWN;
