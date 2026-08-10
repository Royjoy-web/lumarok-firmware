#pragma once
// SensorTaskV2_P2.h — Phase 2 additions to SensorTaskV2
// Include this file INSTEAD of SensorTaskV2.h in the Phase 2 firmware build.
// Builds on top of Phase 1 by adding: RGBW, HVAC loop, NFC, Soil,
// CSI presence, DeepIdle, MultiModal auth, OTA delta check.
// All additions gated by Phase 2 feature flags from Phase2Config.h.
#define SENSOR_TASK_P2_ACTIVE   // suppress P1 sensorTaskFnV2 definition
#include "SensorTaskV2.h"    // Phase 1 drivers/includes, P1 function suppressed
#include "Phase2Config.h"
#include "Phase2Drivers.h"
#include "Phase2Advanced.h"
#include "../drivers/EnvLightDriver.h"
#include "../drivers/IRTransmitter.h"
#include "../drivers/FingerprintDriver.h"

void sensorTaskFnV2(void* pvParam) {
    int8_t wdIdx = WatchdogManager::registerTask("SensorTask", 20000);

    // ── Init: Phase 1 drivers ──────────────────────────────────
    DHTDriver::init();
    EnvLightDriver::init();
    IRDriver::init();
    NVSSceneCache::init();

#if ENABLE_MMWAVE
    LD2410Driver::init();
    LD2410Driver::configure(6, 5);
#elif US1_AVAILABLE
    UltrasonicHAL::init(PIN_US1_TRIG, PIN_US1_ECHO);
#endif
#if US3_AVAILABLE
    UltrasonicHAL::init(PIN_US3_TRIG, PIN_US3_ECHO);
#endif
#if US2_AVAILABLE
    UltrasonicHAL::init(PIN_US2_TRIG, PIN_US2_ECHO);
#endif
#if ENABLE_IR_TX
    IRTransmitter::init();
#endif
#if ENABLE_FINGERPRINT
    static bool fpReady = false;
    fpReady = FingerprintDriver::init();
    if (!fpReady) LOG_W("FP", "Fingerprint sensor absent — scan loop disabled for this session");
#endif
#if ENABLE_PZEM
    PZEM004TDriver::init();
#endif
#if ENABLE_SCD41
    SCD41Driver::init();
#if ENABLE_AIR_QUALITY
    extern TwoWire Wire1;
    AirQualityDriver::init(Wire1);
#if ENABLE_NFC
    NFC_PN532Driver::init(Wire1);
#endif
#endif
#endif
#if ENABLE_WATER_LEAK
    WaterLeakDriver::init();
#endif
#if ENABLE_SMOKE
    SmokeDriver::init();
#endif
#if ENABLE_VIBRATION
    VibrationDriver::init();
#endif
#if ENABLE_WIEGAND
    WiegandDriver::init();
#endif
#if ENABLE_SUNRISE
    SunriseSunset::init();
#endif

    // ── Init: Phase 2 drivers ──────────────────────────────────
#if ENABLE_RGBW
    RGBWDriver::init();
    RGBWDriver::sceneDay(); // default to daylight on boot
#endif
#if ENABLE_HVAC
    HVACController::init();
#endif
#if ENABLE_SOIL
    SoilDriver::init();
#endif
#if ENABLE_MULTIMODAL
    MultiModalAuth::init(0x07); // FP + NFC + PIN all enabled
#endif
#if ENABLE_CSI_PRESENCE
    CSIPresence::init();
#endif
#if ENABLE_DEEP_IDLE
    DeepIdleManager::init();
#endif
#if ENABLE_WIFI_ROAMING
    WiFiRoaming::init();
#endif
#if ENABLE_OTA_DELTA
    OTADeltaManager::init();
#endif

    // ── Timers ─────────────────────────────────────────────────
    unsigned long lastDHT   = 0, lastUS    = 0, lastIR    = 0;
    unsigned long lastPZEM  = 0, lastSun   = 0, lastSoil  = 0;
    unsigned long lastHVAC  = 0, lastIdle  = 0;
    uint16_t seq = 0;

    static char tempId[32], humId[32];
    snprintf(tempId, sizeof(tempId), "%s_living_room_temperature", Identity::get().c_str());
    snprintf(humId,  sizeof(humId),  "%s_living_room_humidity",    Identity::get().c_str());

    LOG_I("SensorV2-P2", "Phase 2 task running — core %d", (int)xPortGetCoreID());

    for (;;) {
        WatchdogManager::checkin(wdIdx);
        unsigned long now = millis();
        bool safeMode = FaultManager::isSafeMode();

        // ── Safety-critical — always run ────────────────────────
#if ENABLE_WATER_LEAK
        { bool leak = WaterLeakDriver::check(); SafetyService::onWaterLeak(leak); }
#endif
#if ENABLE_SMOKE
        { SmokeDriver::Reading sr = SmokeDriver::read(); if (SmokeDriver::tick(sr)) SafetyService::onSmoke(sr.smokeAlert, sr.coAlert, sr.mq7, sr.mq135); }
#endif
#if ENABLE_VIBRATION
        if (VibrationDriver::triggered()) SafetyService::onVibration();
#endif

        if (safeMode) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

        // ── IR beam (100ms) ────────────────────────────────────
        if (now - lastIR >= IR_CHECK_MS) {
            lastIR = now;
            AutomationService::onIRBeam(IRDriver::isBeamBroken());
        }

        // ── RGBW party mode tick (500ms) ──────────────────────
#if ENABLE_RGBW
        RGBWDriver::tick();
#endif

        // ── Multi-modal auth timeout tick ─────────────────────
#if ENABLE_MULTIMODAL
        MultiModalAuth::tick();
#endif

        // ── Wiegand card ──────────────────────────────────────
#if ENABLE_WIEGAND
        if (WiegandDriver::available()) {
            uint32_t code = WiegandDriver::getCode();
            bool auth = WiegandDriver::isAuthorised(code);
            SafetyService::onWiegandCard(code, auth);
#if ENABLE_MULTIMODAL
            if (auth) MultiModalAuth::grantFactor(MultiModalAuth::Factor::NFC, "wiegand");
#endif
        }
#endif

        // ── NFC tap (300ms poll) ──────────────────────────────
#if ENABLE_NFC
        {
            static unsigned long lastNFCMs = 0;
            if (now - lastNFCMs >= 300) {
                lastNFCMs = now;
                NFC_PN532Driver::Tag tag = NFC_PN532Driver::scan();
                if (tag.valid) {
                    bool auth = NFC_PN532Driver::isAuthorised(tag);
                    if (auth) {
#if ENABLE_MULTIMODAL
                        char uidStr[20]; NFC_PN532Driver::uidToString(tag, uidStr, sizeof(uidStr));
                        MultiModalAuth::grantFactor(MultiModalAuth::Factor::NFC, uidStr);
#else
                        SafetyService::onWiegandCard(0, true); // reuse Wiegand unlock path
#endif
                        LOG_I("NFC", "Tag authorised");
                    } else {
                        SafetyService::onWiegandCard(0, false);
                    }
                }
            }
        }
#endif

        // ── Fingerprint scan (500ms) ──────────────────────────
#if ENABLE_FINGERPRINT
        if (fpReady) {
            static unsigned long lastFPMs = 0;
            if (now - lastFPMs >= FINGERPRINT_INTERVAL_MS) {
                lastFPMs = now;
                // authenticate() already posts BIOMETRIC_AUTH_OK/FAIL to
                // EventBus internally — no separate SafetyService call needed.
                int8_t fpId = FingerprintDriver::authenticate(1);
#if ENABLE_MULTIMODAL
                if (fpId >= 0) {
                    char idStr[8];
                    snprintf(idStr, sizeof(idStr), "%d", fpId);
                    MultiModalAuth::grantFactor(MultiModalAuth::Factor::FINGERPRINT, idStr);
                }
#endif
            }
        }
#endif

        // ── mmWave / US occupancy (5s) ─────────────────────────
        if (now - lastUS >= US_READ_MS) {
            lastUS = now;
#if ENABLE_MMWAVE
            LD2410Driver::Reading ld = LD2410Driver::read();
            if (ld.valid) {
                AutomationService::onMmWavePresence(ld.presence, ld.stationary, ld.moving);
                // Publish mmWave presence to MQTT for Spatial view
                SensorReading pr{};
                snprintf(pr.sensor_id, sizeof(pr.sensor_id), "%s_living_room_presence", Identity::get().c_str());
                strlcpy(pr.room, "living_room", sizeof(pr.room));
                strlcpy(pr.name, "presence",   sizeof(pr.name));
                strlcpy(pr.unit, "",           sizeof(pr.unit));
                pr.value      = ld.presence ? 1.0f : 0.0f;
                pr.timestamp  = TimeSync::bestEffort();
                pr.quality_ok = ld.valid;
                pr.seq        = ++seq;
                BatchPublisher::stage(pr);
            }
#elif US1_AVAILABLE
            float us1 = UltrasonicHAL::readCm(PIN_US1_TRIG, PIN_US1_ECHO);
            AutomationService::onOccupancy(us1);
#endif
#if ENABLE_CSI_PRESENCE
            // Supplement mmWave with CSI for rooms without sensors
            bool csiMotion = CSIPresence::motionDetected();
            if (csiMotion) LOG_D("CSI", "Motion via WiFi CSI — var=%.1f", CSIPresence::lastVariance());
#endif
        }

        // ── DHT + SCD41 + IAQ (10s) ───────────────────────────
        if (now - lastDHT >= SENSOR_READ_MS) {
            lastDHT = now;
            DHTReading dht = DHTDriver::read();
            if (dht.valid) {
                FaultManager::clear(FaultCode::DHT_READ_FAIL);
                SafetyService::onTemperature(dht.temperature);
                time_t ts = TimeSync::bestEffort();
                bool uncertain = (!ts);
                if (!ts) ts = (time_t)(now / 1000UL);
                SensorReading r{};
                r.timestamp = ts; r.ts_uncertain = uncertain; r.quality_ok = true;
                r.error_count = dht.errorCount;
                strlcpy(r.sensor_id, tempId,       sizeof(r.sensor_id));
                strlcpy(r.room,      "living_room", sizeof(r.room));
                strlcpy(r.name, "temperature", sizeof(r.name)); strlcpy(r.unit,"C",sizeof(r.unit));
                r.value = dht.temperature; r.seq = ++seq;
                BatchPublisher::stage(r);
                strlcpy(r.name, "humidity", sizeof(r.name)); strlcpy(r.unit,"%",sizeof(r.unit));
                r.value = dht.humidity; r.seq = ++seq;
                BatchPublisher::stage(r);
            } else {
                // FIX (fault cascade): a permanently-absent DHT sensor used
                // to escalate to SEV_HIGH after 5 failures — but a single
                // active HIGH-severity fault alone is enough to push the
                // whole SystemMode to SAFE (see FaultManager::_evaluateMode,
                // high>=1 check), which then makes SensorTaskV2's loop
                // skip everything else via its `if (safeMode) continue;`
                // guard. One optional, unwired sensor shouldn't be able to
                // silently disable every other sensor in this task. Capped
                // at MEDIUM: still recorded/visible in diagnostics, but a
                // lone DHT fault can't reach DEGRADED (needs medium>=2) or
                // SAFE (needs medium>=4) on its own.
                FaultManager::record(FaultCode::DHT_READ_FAIL, "nan", FaultSeverity::SEV_MEDIUM);
            }

            // ── LDR + thermistor (same 10s cadence as DHT) ────
            {
                time_t ts = TimeSync::bestEffort();
                bool uncertain = (!ts);
                if (!ts) ts = (time_t)(now / 1000UL);
                SensorReading r{};
                r.timestamp = ts; r.ts_uncertain = uncertain; r.quality_ok = true;
                strlcpy(r.room, "living_room", sizeof(r.room));

                strlcpy(r.sensor_id, "living_room_light", sizeof(r.sensor_id));
                strlcpy(r.name, "light", sizeof(r.name)); strlcpy(r.unit, "%", sizeof(r.unit));
                r.value = EnvLightDriver::lightPercent(); r.seq = ++seq;
                BatchPublisher::stage(r);

                strlcpy(r.sensor_id, "living_room_thermistor", sizeof(r.sensor_id));
                strlcpy(r.name, "temperature_aux", sizeof(r.name)); strlcpy(r.unit, "C", sizeof(r.unit));
                r.value = EnvLightDriver::thermistorC(); r.seq = ++seq;
                BatchPublisher::stage(r);
            }

#if ENABLE_SCD41
            {
                SCD41Driver::Reading iaq = SCD41Driver::read();
                if (iaq.valid) {
                    SafetyService::onCO2(iaq.co2);
                    SCD41Driver::publishAll(iaq);
#if ENABLE_HVAC
                    HVACController::thermostatTick(iaq.temp);
#endif
#if ENABLE_AIR_QUALITY
                    AirQualityDriver::compensate(iaq.temp, iaq.humidity);
                    AirQualityDriver::Reading aqr = AirQualityDriver::read();
                    AirQualityDriver::publishAll(aqr);
#endif
                }
            }
#endif
    
#if ENABLE_PZEM
        {
            auto pz = PZEM004TDriver::read();
            if (pz.valid) {
                SensorReading r{};
                r.priority = 2; r.seq = ++seq;
                strlcpy(r.unit, "W", sizeof(r.unit));

                strlcpy(r.name, "pzem_power",   sizeof(r.name)); r.value = pz.power;   BatchPublisher::stage(r);
                strlcpy(r.name, "pzem_voltage", sizeof(r.name)); r.value = pz.voltage; BatchPublisher::stage(r);
                strlcpy(r.name, "pzem_current", sizeof(r.name)); r.value = pz.current; strlcpy(r.unit,"A",sizeof(r.unit)); BatchPublisher::stage(r);
                strlcpy(r.name, "pzem_kwh",     sizeof(r.name)); r.value = pz.energy;  strlcpy(r.unit,"kWh",sizeof(r.unit)); BatchPublisher::stage(r);
                strlcpy(r.name, "pzem_pf",      sizeof(r.name)); r.value = pz.pf;      strlcpy(r.unit,"",sizeof(r.unit)); BatchPublisher::stage(r);

                // High-power alert (>3500W = ~15A on 230V)
                if (pz.power > 3500.0f) {
                    Event e{}; e.type = EventType::SAFETY_ALERT;
                    strlcpy(e.data.alert.type, "pzem_overload", sizeof(e.data.alert.type));
                    snprintf(e.data.alert.message, sizeof(e.data.alert.message),
                             "power=%.0fW", pz.power);
                    e.data.alert.device[0] = '\0';
                    e.data.alert.severity  = AlertSeverity::WARN;
                    e.data.alert.timestamp = 0;
                    EventBus::postAlert(e);
                }
            } else {
                FaultManager::record(FaultCode::SENSOR_READ_FAIL, "pzem", FaultSeverity::SEV_LOW);
            }
        }
#endif // ENABLE_PZEM
        BatchPublisher::flush();
        }

        // ── Soil moisture (30s) ────────────────────────────────
#if ENABLE_SOIL
        if (now - lastSoil >= 30000UL) {
            lastSoil = now;
            uint8_t pct = SoilDriver::readPercent();
            // Publish moisture reading
            SensorReading sr{};
            snprintf(sr.sensor_id, sizeof(sr.sensor_id), "%s_garden_soil", Identity::get().c_str());
            strlcpy(sr.room, "garden",         sizeof(sr.room));
            strlcpy(sr.name, "soil_moisture",  sizeof(sr.name));
            strlcpy(sr.unit, "%",              sizeof(sr.unit));
            sr.value = pct; sr.timestamp = TimeSync::bestEffort(); sr.seq = ++seq; sr.quality_ok = true;
            BatchPublisher::stage(sr);
            // Auto-pump trigger
            if (SoilDriver::needsWatering()) {
                Event e{}; e.type = EventType::COMMAND_RECEIVED;
                strlcpy(e.data.command.room,        "garden",    sizeof(e.data.command.room));
                strlcpy(e.data.command.device_name, "pool_pump", sizeof(e.data.command.device_name));
                e.data.command.action = CommandAction::ON;
                e.data.command.value  = -1;
                EventBus::postCommand(e);
                LOG_I("Soil", "Auto-watering triggered: %d%%", pct);
            }
            BatchPublisher::flush();
        }
#endif

        // ── PZEM power (60s) ──────────────────────────────────
#if ENABLE_PZEM
        if (now - lastPZEM >= PZEM_PUBLISH_MS) {
            lastPZEM = now;
            PZEM004TDriver::publishAll("electrical");
            BatchPublisher::flush();
        }
#endif

        // ── Sunrise tick (60s) ────────────────────────────────
#if ENABLE_SUNRISE
        if (now - lastSun >= 60000UL) { lastSun = now; SunriseSunset::tick(); }
#endif

        // ── Deep idle (end of loop, when nothing active) ──────
#if ENABLE_DEEP_IDLE
        lastIdle = now;
        DeepIdleManager::idleIfQuiet(
            max({lastDHT, lastUS, lastIR, lastPZEM, lastSoil, lastSun}), now
        );
#endif

        vTaskDelay(pdMS_TO_TICKS(50)); // 50ms base loop (was 100ms)
    }
}
