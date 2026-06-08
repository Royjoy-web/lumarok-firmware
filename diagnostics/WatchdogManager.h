#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include "../core/Types.h"
#include "../core/Config.h"

// ─────────────────────────────────────────────────────────────
// WatchdogManager — tracks per-task liveness independently of
// the hardware watchdog, providing soft detection before the
// hardware WDT fires and reboots without any diagnostics.
//
// Each registered task calls check-in periodically.
// WatchdogManager detects missed check-ins and can log them,
// trigger alerts, or escalate to a hard reset via the hardware WDT
// by deliberately not feeding it.
// ─────────────────────────────────────────────────────────────

#define WDM_MAX_TASKS  10

struct TaskHealth {
    char          name[20];
    TaskHandle_t  handle;
    uint32_t      timeoutMs;
    unsigned long lastCheckinMs;
    uint32_t      checkinCount;
    uint32_t      missedCount;
    bool          registered;
    bool          enabled;
};

class WatchdogManager {
public:
    static void init() {
        _mutex = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        memset(_tasks, 0, sizeof(_tasks));
        _count = 0;
        LOG_I("WDM", "Watchdog manager initialised");
    }

    // Register a task for soft watchdog monitoring.
    // Returns a slot index to pass to checkin().
    // BUG-D fix: mutex-protected — tasks on both cores call registerTask()
    // concurrently at startup. Without the lock two tasks can read the same
    // _count, claim the same slot, and one registration is silently lost
    // (and its hardware WDT entry is never added).
    static int8_t registerTask(const char* name, uint32_t timeoutMs) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_count >= WDM_MAX_TASKS) {
            xSemaphoreGive(_mutex);
            LOG_E("WDM", "Max tasks reached");
            return -1;
        }
        int8_t idx = _count++;
        TaskHealth& t = _tasks[idx];
        strlcpy(t.name,       name,      sizeof(t.name));
        t.handle         = xTaskGetCurrentTaskHandle();
        t.timeoutMs      = timeoutMs;
        t.lastCheckinMs  = millis();
        t.checkinCount   = 0;
        t.missedCount    = 0;
        t.registered     = true;
        t.enabled        = true;
        xSemaphoreGive(_mutex);

        // Hardware WDT registration outside the lock — esp_task_wdt_add()
        // is thread-safe (IDF internal spinlock) and can be slow.
        esp_task_wdt_add(t.handle);
        LOG_I("WDM", "Registered: %s (timeout=%ums)", name, (unsigned)timeoutMs);
        return idx;
    }

    // Task heartbeat — call from within the task's main loop
    static void checkin(int8_t idx) {
        if (idx < 0 || idx >= _count) return;
        TaskHealth& t      = _tasks[idx];
        t.lastCheckinMs    = millis();
        t.checkinCount++;
        esp_task_wdt_reset();  // Feed hardware WDT
    }

    // Monitor pass — call from DiagnosticsTask every second
    static void scan() {
        unsigned long now = millis();
        for (int8_t i = 0; i < _count; i++) {
            TaskHealth& t = _tasks[i];
            if (!t.registered || !t.enabled) continue;

            uint32_t age = (uint32_t)(now - t.lastCheckinMs);
            if (age > t.timeoutMs) {
                t.missedCount++;
                LOG_W("WDM", "MISSED CHECKIN: %s  age=%ums  missed=%u",
                      t.name, (unsigned)age, (unsigned)t.missedCount);

                // Escalate: 3 missed → force hardware WDT by starving it
                if (t.missedCount >= 3) {
                    LOG_E("WDM", "ESCALATING: task %s unresponsive — watchdog restart", t.name);
                    // Deliberately do NOT feed hardware WDT for this task
                    // esp_task_wdt will fire within WATCHDOG_TIMEOUT_S
                    t.enabled = false;  // stop tracking; WDT will handle it
                }
            }
        }
    }

    // Temporarily suspend WDT for a task (e.g. during OTA)
    static void suspend(int8_t idx) {
        if (idx < 0 || idx >= _count) return;
        esp_task_wdt_delete(_tasks[idx].handle);
        _tasks[idx].enabled = false;
        LOG_I("WDM", "Suspended: %s", _tasks[idx].name);
    }

    static void resume(int8_t idx) {
        if (idx < 0 || idx >= _count) return;
        TaskHealth& t = _tasks[idx];
        esp_task_wdt_add(t.handle);
        t.lastCheckinMs = millis();
        t.missedCount   = 0;
        t.enabled       = true;
        LOG_I("WDM", "Resumed: %s", t.name);
    }

    // Serialise health summary for diagnostics publish
    static void buildHealthJSON(char* buf, size_t len) {
        size_t pos = 0;
        pos += snprintf(buf + pos, len - pos, "{\"tasks\":{");
        for (int8_t i = 0; i < _count; i++) {
            TaskHealth& t  = _tasks[i];
            uint32_t    wm = (uint32_t)uxTaskGetStackHighWaterMark(t.handle);
            pos += snprintf(buf + pos, len - pos,
                "\"%s\":{\"missed\":%u,\"checkins\":%u,\"stack_hwm\":%u,\"ok\":%s}%s",
                t.name, (unsigned)t.missedCount, (unsigned)t.checkinCount,
                (unsigned)wm, t.enabled ? "true" : "false",
                (i < _count - 1) ? "," : "");
        }
        snprintf(buf + pos, len - pos, "}}");
    }

    static uint8_t    count()          { return _count; }
    static TaskHealth* task(int8_t i)  { return (i < _count) ? &_tasks[i] : nullptr; }

private:
    static TaskHealth _tasks[WDM_MAX_TASKS];
    static int8_t     _count;
    static SemaphoreHandle_t _mutex;
};

inline TaskHealth WatchdogManager::_tasks[WDM_MAX_TASKS] = {};
inline int8_t     WatchdogManager::_count                = 0;
inline SemaphoreHandle_t WatchdogManager::_mutex         = nullptr;
