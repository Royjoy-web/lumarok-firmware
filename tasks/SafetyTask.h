#pragma once
#include "../core/Identity.h"
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../diagnostics/WatchdogManager.h"
#include "../automation/SafetyService.h"
#include "../drivers/GasDriver.h"
#include "../drivers/PCF8574Driver.h"
#include "../hal/UltrasonicHAL.h"
#include "../core/Config.h"
#include "../telemetry/BatchPublisher.h"   // BUG-1 fix: bypass orphaned _sensorQ
#include "../telemetry/TimeSync.h"         // BUG-7 fix: proper timestamp

// Runs on Core 1, priority 7 — highest in system.
// Gas, door, window checks run here independently of all other tasks.
// FIX v3.2: Registers with WatchdogManager instead of hardware-WDT only.
// This makes SafetyTask stack watermarks visible in diagnostics JSON and
// enables soft-watchdog detection before the hardware WDT fires blindly.
void safetyTaskFn(void* pvParam) {
    // 5s timeout — safety loops at 50ms base cadence; 100 consecutive
    // misses indicates genuine blockage in gas/door/PCF8574 read paths
    int8_t wdIdx = WatchdogManager::registerTask("SafetyTask", 5000);
    SafetyService::init();
    GasDriver::init();
    PCF8574Driver::init();

    bool prevDoor   = false;
    bool prevWindow = false;

    unsigned long lastGasMs    = 0;
    unsigned long lastDoorMs   = 0;
    unsigned long lastSafetyMs = 0;
    static uint16_t gasSensorSeq = 0;   // BUG-7: monotonic sequence counter
    static char gasId[40];
    snprintf(gasId, sizeof(gasId), "%s_kitchen_gas", Identity::get().c_str());

    LOG_I("SafetyTask", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        WatchdogManager::checkin(wdIdx);
        unsigned long now = millis();

        // ── Gas check every GAS_CHECK_MS ─────────────────────
        if (now - lastGasMs >= GAS_CHECK_MS) {
            lastGasMs = now;
            GasReading g = GasDriver::read();
            SafetyService::onGasReading(g.analog, g.digitalAlert);

            // BUG-1 fix: post directly to BatchPublisher — bypass _sensorQ which
            //   has no consumer and fills in 96 s, silently dropping all readings.
            // BUG-7 fix: set timestamp, ts_uncertain, seq to match SensorTaskV2.
            SensorReading r{};
            time_t ts        = TimeSync::bestEffort();
            bool   uncertain = (ts == 0);
            if (!ts) ts      = (time_t)(millis() / 1000UL);
            r.timestamp    = ts;
            r.ts_uncertain = uncertain;
            r.seq          = ++gasSensorSeq;
            r.value        = (float)g.analog;
            r.quality_ok   = g.valid;
            strlcpy(r.sensor_id, gasId,      sizeof(r.sensor_id));
            strlcpy(r.room,      "kitchen",  sizeof(r.room));
            strlcpy(r.name,      "gas",      sizeof(r.name));
            strlcpy(r.unit,      "raw",      sizeof(r.unit));
            // OPP-1: hardware comparator fired → priority-1 (immediate flush, bypasses 8s window)
            BatchPublisher::stage(r, g.digitalAlert ? 1 : 0);
        }

        // ── Door / window check every DOOR_CHECK_MS ───────────
        if (now - lastDoorMs >= DOOR_CHECK_MS) {
            lastDoorMs = now;
            SafetyService::onDoorState  (PCF8574Driver::isDoorOpen(),   prevDoor);
            SafetyService::onWindowState(PCF8574Driver::isWindowOpen(), prevWindow);
        }

        // ── Max-runtime safety enforcement ────────────────────
        if (now - lastSafetyMs >= SAFETY_CHECK_MS) {
            lastSafetyMs = now;
            SafetyService::tick(now);
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
