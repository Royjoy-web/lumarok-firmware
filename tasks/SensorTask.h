// ⚠️ DEPRECATED — superseded by the V2 module. Not included in any .ino build (see includes). Kept only for reference; safe to ignore/remove in a future cleanup PR.

#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../core/Config.h"
#include "../drivers/DHTDriver.h"
#include "../drivers/IRDriver.h"
#include "../hal/UltrasonicHAL.h"
#include "../hal/StepperHAL.h"
#include "../automation/AutomationService.h"
#include "../automation/SafetyService.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../telemetry/TimeSync.h"

void sensorTaskFn(void* pvParam) {
    TaskManager::registerCurrentTask();
    DHTDriver::init();
    IRDriver::init();
    UltrasonicHAL::init(PIN_US1_TRIG, PIN_US1_ECHO);
    UltrasonicHAL::init(PIN_US3_TRIG, PIN_US3_ECHO);
#if US2_AVAILABLE
    UltrasonicHAL::init(PIN_US2_TRIG, PIN_US2_ECHO);
#endif
    AutomationService::init();

    unsigned long lastDHTMs  = 0;
    unsigned long lastUSMs   = 0;
    unsigned long lastIRMs   = 0;
    static uint16_t seq      = 0;

    LOG_I("SensorTask", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        TaskManager::feedWatchdog();
        unsigned long now = millis();

        // ── IR beam (100ms) ───────────────────────────────────
        if (now - lastIRMs >= IR_CHECK_MS) {
            lastIRMs = now;
            bool broken = IRDriver::isBeamBroken();
            AutomationService::onIRBeam(broken);
        }

        // ── Ultrasonic (5s) ───────────────────────────────────
        if (now - lastUSMs >= US_READ_MS) {
            lastUSMs = now;

            float us1 = UltrasonicHAL::readCm(PIN_US1_TRIG, PIN_US1_ECHO);
            AutomationService::onOccupancy(us1);

#if US2_AVAILABLE
            float us2 = UltrasonicHAL::readCm(PIN_US2_TRIG, PIN_US2_ECHO);
            AutomationService::onParking(us2);
#endif
            // Gate obstacle check if stepper is moving
            if (StepperHAL::isMoving()) {
                float us3 = UltrasonicHAL::readCm(PIN_US3_TRIG, PIN_US3_ECHO);
                if (SafetyService::isGateObstacle(us3)) StepperHAL::emergencyStop();
            }
        }

        // ── DHT22 + publish batch (10s) ───────────────────────
        if (now - lastDHTMs >= SENSOR_READ_MS) {
            lastDHTMs = now;
            DHTReading dht = DHTDriver::read();

            if (dht.valid) {
                SafetyService::onTemperature(dht.temperature);
                time_t ts      = TimeSync::bestEffort();
                bool uncertain = (ts == 0);
                if (!ts) ts    = (time_t)(now / 1000);

                auto _pub = [&](const char* name, const char* unit, float val) {
                    SensorReading r{};
                    strlcpy(r.room,      "living_room", sizeof(r.room));
                    strlcpy(r.name,      name,          sizeof(r.name));
                    strlcpy(r.sensor_id, "FILL_AFTER_SETUP", sizeof(r.sensor_id));
                    strlcpy(r.unit,      unit,          sizeof(r.unit));
                    r.value       = val;
                    r.timestamp   = ts;
                    r.ts_uncertain= uncertain;
                    r.seq         = ++seq;
                    r.quality_ok  = true;
                    r.error_count = dht.errorCount;
                    TelemetryPipeline::publish(r);
                };

                _pub("temperature", "C",  dht.temperature);
                _pub("humidity",    "%",  dht.humidity);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
