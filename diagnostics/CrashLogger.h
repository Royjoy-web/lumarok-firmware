#pragma once
#include <esp_system.h>
#include "../core/Types.h"
#include "../storage/NVSStore.h"
#include "../core/Config.h"

#define NVS_KEY_CRASH_RECORD "crash_rec"

class CrashLogger {
public:
    static void init() {
        // Register a panic handler to capture crash context before reset
        // Note: limited operations available inside panic handler
        esp_register_shutdown_handler(_onShutdown);
        _loadLastCrash();
    }

    // Called at boot by DiagnosticsManager to publish and clear the record
    static bool hasCrashRecord() { return _last.valid; }

    static const CrashRecord& lastCrash() { return _last; }

    static void clearCrashRecord() {
        _last.valid = false;
        NVSStore::remove(NVS_NS_DIAG, NVS_KEY_CRASH_RECORD);
    }

    // Store the current topic being processed — called by MQTTRxTask
    static void setLastTopic(const char* topic) {
        strlcpy(_currentTopic, topic, sizeof(_currentTopic));
    }

    static void setCurrentTask(const char* task) {
        strlcpy(_currentTask, task, sizeof(_currentTask));
    }

private:
    static void _onShutdown() {
        // This runs before reset — keep it minimal
        esp_reset_reason_t reason = esp_reset_reason();
        bool unexpected = (reason == ESP_RST_PANIC    ||
                           reason == ESP_RST_INT_WDT   ||
                           reason == ESP_RST_TASK_WDT  ||
                           reason == ESP_RST_WDT);
        if (!unexpected) return;

        CrashRecord rec{};
        rec.crash_reason = (uint32_t)reason;
        rec.uptime_s     = millis() / 1000UL;
        rec.free_heap    = ESP.getFreeHeap();
        rec.crash_count  = NVSStore::getU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, 0) + 1;
        rec.valid        = true;
        strlcpy(rec.task_name,  _currentTask, sizeof(rec.task_name));
        strlcpy(rec.last_topic, _currentTopic, sizeof(rec.last_topic));

        NVSStore::putBlob(NVS_NS_DIAG, NVS_KEY_CRASH_RECORD, &rec, sizeof(rec));
        NVSStore::putU8(NVS_NS_CONFIG, NVS_KEY_CRASH_CNT, rec.crash_count);
    }

    static void _loadLastCrash() {
        _last.valid = false;
        CrashRecord rec{};
        if (NVSStore::getBlob(NVS_NS_DIAG, NVS_KEY_CRASH_RECORD,
                               &rec, sizeof(rec)) == sizeof(rec)) {
            _last = rec;
        }
    }

    static CrashRecord _last;
    static char        _currentTopic[64];
    static char        _currentTask[16];
};

inline CrashRecord CrashLogger::_last             = {};
inline char        CrashLogger::_currentTopic[64] = {};
inline char        CrashLogger::_currentTask[16]  = "unknown";
