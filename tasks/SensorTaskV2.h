#pragma once
#include "../core/Identity.h"
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../core/Config.h"
#include "../drivers/DHTDriver.h"
#include "../drivers/IRDriver.h"
#include "../drivers/PCF8574Driver.h"
#include "../hal/UltrasonicHAL.h"
#include "../hal/StepperHAL.h"
#include "../automation/AutomationService.h"
#include "../automation/SafetyService.h"
#include "../telemetry/BatchPublisher.h"
#include "../telemetry/TimeSync.h"
#include "../diagnostics/FaultManager.h"
#include "../diagnostics/WatchdogManager.h"

#ifndef SENSOR_TASK_P2_ACTIVE
void sensorTaskFnV2(void* pvParam) {
    int8_t wdIdx = WatchdogManager::registerTask("SensorTask", 20000);

    DHTDriver::init();
    IRDriver::init();
    UltrasonicHAL::init(PIN_US1_TRIG, PIN_US1_ECHO);
    UltrasonicHAL::init(PIN_US3_TRIG, PIN_US3_ECHO);
#if US2_AVAILABLE
    UltrasonicHAL::init(PIN_US2_TRIG, PIN_US2_ECHO);
#endif

    unsigned long lastDHTMs = 0, lastUSMs = 0, lastIRMs = 0;
    static uint16_t seq = 0;

    // Pre-built sensor IDs — no runtime String allocation
    static char tempId[32], humId[32];
    snprintf(tempId, sizeof(tempId), "%s_living_room_temperature", Identity::get().c_str());
    snprintf(humId,  sizeof(humId),  "%s_living_room_humidity",    Identity::get().c_str());

    LOG_I("SensorV2", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        WatchdogManager::checkin(wdIdx);
        unsigned long now = millis();

        // ── Skip non-safety sensors in SAFE/FAILED mode ───────
        bool safeMode = FaultManager::isSafeMode();

        // ── IR beam (100ms) — always run ──────────────────────
        if (now - lastIRMs >= IR_CHECK_MS) {
            lastIRMs = now;
            AutomationService::onIRBeam(IRDriver::isBeamBroken());
        }

        if (safeMode) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        // ── Ultrasonic (5s) ───────────────────────────────────
        if (now - lastUSMs >= US_READ_MS) {
            lastUSMs = now;
            float us1 = UltrasonicHAL::readCm(PIN_US1_TRIG, PIN_US1_ECHO);
            AutomationService::onOccupancy(us1);
#if US2_AVAILABLE
            float us2 = UltrasonicHAL::readCm(PIN_US2_TRIG, PIN_US2_ECHO);
            AutomationService::onParking(us2);
#endif
            if (StepperHAL::isMoving()) {
                float us3 = UltrasonicHAL::readCm(PIN_US3_TRIG, PIN_US3_ECHO);
                if (SafetyService::isGateObstacle(us3)) {
                    StepperHAL::emergencyStop();
                    FaultManager::record(FaultCode::STEPPER_FAULT, "obstacle", FaultSeverity::SEV_MEDIUM);
                }
            }
        }

        // ── DHT22 (10s) ───────────────────────────────────────
        if (now - lastDHTMs >= SENSOR_READ_MS) {
            lastDHTMs = now;
            DHTReading dht = DHTDriver::read();

            if (dht.valid) {
                FaultManager::clear(FaultCode::DHT_READ_FAIL);
                SafetyService::onTemperature(dht.temperature);

                time_t ts       = TimeSync::bestEffort();
                bool   uncertain = (ts == 0);
                if (!ts) ts      = (time_t)(now / 1000UL);

                // Reuse stack-allocated SensorReading — no heap
                SensorReading r{};
                r.timestamp    = ts;
                r.ts_uncertain = uncertain;
                r.seq          = ++seq;
                r.quality_ok   = true;
                r.error_count  = dht.errorCount;
                strlcpy(r.sensor_id, tempId,       sizeof(r.sensor_id));
                strlcpy(r.room,      "living_room", sizeof(r.room));

                strlcpy(r.name, "temperature", sizeof(r.name));
                strlcpy(r.unit, "C",           sizeof(r.unit));
                r.value = dht.temperature;
                r.seq   = ++seq;   // BUG-FIX: seq must differ per reading; both used same seq
                BatchPublisher::stage(r);

                strlcpy(r.name, "humidity", sizeof(r.name));
                strlcpy(r.unit, "%",        sizeof(r.unit));
                r.value = dht.humidity;
                r.seq   = ++seq;   // unique seq for humidity
                BatchPublisher::stage(r);

            } else {
                FaultManager::record(FaultCode::DHT_READ_FAIL, "nan",
                    dht.consecutiveErrors > 5 ? FaultSeverity::SEV_HIGH : FaultSeverity::SEV_LOW);
                LOG_W("SensorV2", "DHT fail #%d", (int)dht.consecutiveErrors);
            }

            // Flush any pending batch on this cycle
            BatchPublisher::flush();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
#endif // SENSOR_TASK_P2_ACTIVE
