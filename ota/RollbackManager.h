#pragma once
#include <Update.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../storage/NVSStore.h"
#include "../core/BootManager.h"

class RollbackManager {
public:
    // Called in setup() before tasks start — check if we should roll back
    static void checkAndRollback() {
        uint8_t crashCnt = NVSStore::getU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0);
        // BUG-5 fix: use ROLLBACK_CRASH_THRESHOLD from Config.h, not a magic number.
        // Intentional sequencing: rollback fires at 3, safe mode at SAFE_MODE_CRASH_THRESHOLD=5.
        if (crashCnt >= ROLLBACK_CRASH_THRESHOLD && Update.canRollBack()) {
            LOG_W("Rollback", "Crash loop detected (count=%d) — rolling back", (int)crashCnt);
            _publishRollbackAttempt();
            NVSStore::putU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0);
            delay(500);
            Update.rollBack();
            esp_restart();
        }
    }

    // Called immediately after a successful OTA — stores version info
    static void markOTAComplete(const char* newVersion) {
        String prev = NVSStore::getString(NVS_NS_CONFIG, NVS_KEY_FW_VERSION, FIRMWARE_VERSION);
        NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_PREV_FW,    prev);
        NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_FW_VERSION, newVersion);
        NVSStore::putU8    (NVS_NS_CONFIG, NVS_KEY_CRASH_CNT,  0);
        LOG_I("Rollback", "OTA complete: %s → %s", prev.c_str(), newVersion);
    }

    // Manual rollback via authenticated MQTT system command
    static bool manualRollback() {
        if (!Update.canRollBack()) {
            LOG_W("Rollback", "Rollback not available");
            return false;
        }
        LOG_I("Rollback", "Manual rollback triggered");
        Update.rollBack();
        esp_restart();
        return true;  // unreachable
    }

    static bool canRollBack() { return Update.canRollBack(); }

private:
    static void _publishRollbackAttempt() {
        // We can't use MQTTTransport here (not yet initialized at boot)
        // Just log to serial; DiagnosticsManager will pick up the crash record
        LOG_W("Rollback", "Rollback triggered — previous FW: %s",
              NVSStore::getString(NVS_NS_CONFIG, NVS_KEY_PREV_FW).c_str());
    }
};
