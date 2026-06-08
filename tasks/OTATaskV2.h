#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../ota/OTAManagerV2.h"
#include "../diagnostics/WatchdogManager.h"
#include "../storage/FlashWearGuard.h"

void otaTaskFnV2(void* pvParam) {
    int8_t wdIdx = WatchdogManager::registerTask("OTATask", WATCHDOG_TIMEOUT_S * 1000);
    OTAManagerV2::init();
    LOG_I("OTAv2", "Ready");

    for (;;) {
        WatchdogManager::checkin(wdIdx);

        Event e{};
        if (!EventBus::waitOTA(e, pdMS_TO_TICKS(5000))) continue;
        if (e.type != EventType::OTA_COMMAND) continue;

        LOG_I("OTAv2", "Starting OTA");
        // Suspend OTA task from soft WDT; hardware WDT extended
        WatchdogManager::suspend(wdIdx);
        TaskManager::extendWatchdog();

        // Flush NVS before potentially long download
        FlashWearGuard::flushAll();

        OTAManagerV2::begin(e.data.ota);
        while (!OTAManagerV2::tick()) {
            TaskManager::feedWatchdog();
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        TaskManager::restoreWatchdog();
        WatchdogManager::resume(wdIdx);
        LOG_I("OTAv2", "Complete (state=%d)", (int)OTAManagerV2::state());
    }
}
