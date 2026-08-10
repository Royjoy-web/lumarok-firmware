// ── ArduinoDroid dependency-scanner workaround ──────────────────
// ArduinoDroid's "Analyzing sketch dependencies" step only detects
// libraries from #include lines in THIS .ino file — not from includes
// nested inside this project's own .h files, however many levels deep.
// Without these direct includes here, ArduinoDroid compiles headers fine
// but never builds librariesBuild/ for these libraries, so every symbol
// from them is "undefined reference" at link time. Harmless under
// PlatformIO (which doesn't have this limitation) — keeping both build
// trees identical avoids drift.
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Daikin.h>
#include <ir_Mitsubishi.h>
#include <ir_Gree.h>
#include <Adafruit_Fingerprint.h>

// PZEM library is only required when ENABLE_PZEM is on (defaults false, set
// via Phase2Config.h normally — but that hasn't been included yet at this
// point in the file, hence the local default here). Matches the same
// ENABLE_PZEM gate already used around the real driver code in
// Phase2Drivers.h. If you don't have a PZEM-004T wired up, leave this off
// and you don't need the library installed at all.
#ifndef ENABLE_PZEM
  #define ENABLE_PZEM false
#endif
#if ENABLE_PZEM
  #include <PZEM004Tv30.h>
#endif

// HIL test override: no physical AS608 fingerprint sensor is wired to this
// test unit. Two independent app-level fixes to FingerprintDriver.h (a
// scan-loop guard, then a full rewrite with UART-driver-install checks,
// readiness tracking, and a mutex) both failed to stop a crash that occurs
// with an identical call-chain every time — meaning the fault sits below
// application code, inside the Adafruit library or ESP-IDF UART internals
// when nothing physically answers on the wire. Root-causing that requires
// hardware-in-the-loop debugging (logic analyzer or a real sensor
// attached), not further blind app-layer patching. Disabling until either
// is available; re-enable once a real AS608 is wired up.
#ifndef ENABLE_FINGERPRINT
  #define ENABLE_FINGERPRINT false
#endif

// ═══════════════════════════════════════════════════════════════
//  LumaRoK C10 — Enterprise Hardened Firmware v4.2.0-P2
// ═══════════════════════════════════════════════════════════════

// ── Core ──────────────────────────────────────────────────────
#include "core/Types.h"
#include "core/Config.h"
#include "core/Identity.h"
#include "core/EventBus.h"
#include "core/BootManager.h"
#include "core/MemoryPool.h"
#include "core/RetryPolicy.h"
#include "core/TaskManagerV2.h"

// ── Storage & Security ────────────────────────────────────────
#include "storage/NVSStore.h"
#include "storage/TelemetryBuffer.h"
#include "storage/PriorityBuffer.h"
#include "storage/FlashWearGuard.h"
#include "security/CredentialStore.h"
#include "security/HMACVerifier.h"

// ── HAL & Drivers ─────────────────────────────────────────────
#include "hal/RelayHAL.h"
#include "hal/StepperHAL.h"
#include "hal/UltrasonicHAL.h"
#include "drivers/DHTDriver.h"
#include "drivers/GasDriver.h"
#include "drivers/IRDriver.h"
#include "drivers/PCF8574Driver.h"

// ── Networking & MQTT ─────────────────────────────────────────
#include "networking/WiFiManager.h"
#include "networking/ReconnectEngine.h"
#include "networking/LocalCommandServer.h"   // Phase 1: local (LAN) control path
#include "mqtt/MQTTTopics.h"
#include "mqtt/MQTTTransport.h"
// Note: CommandDispatcher.h moved below SensorTaskV2_P2.h so Phase2Drivers are available

// ── Telemetry ─────────────────────────────────────────────────
#include "telemetry/TimeSync.h"
#include "telemetry/TelemetryPipeline.h"
#include "telemetry/BatchPublisher.h"

// ── Automation & Safety ───────────────────────────────────────
#include "automation/DeviceRegistry.h"
#include "automation/SafetyService.h"
#include "automation/AutomationService.h"

// ── OTA ───────────────────────────────────────────────────────
#include "ota/OTAVerifier.h"
#include "ota/RollbackManager.h"
#include "ota/OTAManagerV2.h"

// ── Provisioning ──────────────────────────────────────────────
#include "provisioning/ProvisioningManager.h"
#include "provisioning/BLEProvisioning.h"
#include "provisioning/APProvisioning.h"
#include "provisioning/SerialProvisioning.h"

// ── Diagnostics ───────────────────────────────────────────────
#include "diagnostics/CrashLogger.h"
#include "diagnostics/DiagnosticsManager.h"
#include "diagnostics/WatchdogManager.h"
#include "diagnostics/FaultManager.h"

// ── Tasks (V2 hardened) ───────────────────────────────────────
#include "tasks/SafetyTask.h"
#include "tasks/SensorTaskV2_P2.h"   // pulls Phase2Config/Drivers/Advanced internally
#include "mqtt/CommandDispatcher.h"     // after P2 drivers so ENABLE_RGBW flag is resolved
#include "tasks/NetworkTaskV2.h"
#include "tasks/ActuatorTask.h"
#include "tasks/MQTTTasks.h"
#include "tasks/OTATaskV2.h"
#include "tasks/DiagnosticsTaskV2.h"

// ── Global memory pool instances ─────────────────────────────
StaticPool<SensorReading, 32> gSensorPool;
StaticPool<Alert, 8>          gAlertPool;

// ─────────────────────────────────────────────────────────────
void runProvisioningFlow();

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    LOG_I("Main", "LumaRoK v4.2.0-P2 (hardened)  HW:%s", HARDWARE_REVISION);

    // ── 1. Identity ───────────────────────────────────────────
    Identity::init();

    // ── 2. Boot analysis ─────────────────────────────────────
    BootManager::init();

    // ── 3. Rollback gate — before ANY other init ──────────────
    RollbackManager::checkAndRollback();

    // ── 4. Core primitives ────────────────────────────────────
    EventBus::init();
    WatchdogManager::init();
    FaultManager::init();
    FlashWearGuard::init();

    // ── 5. Storage ────────────────────────────────────────────
    PriorityBuffer::init();
    DeviceRegistry::init();
    ProvisioningManager::init();

    // ── 6. Memory pools ───────────────────────────────────────
    gSensorPool.init();
    gAlertPool.init();

    // FIX (startup race): BatchPublisher's mutex must exist before ANY
    // task can call stage()/flush(). It was previously only created inside
    // NetworkTaskV2's networkTaskFnV2(), behind a deliberate 3s settle
    // delay (see FIX (heap audit) comment there) — but SensorTaskV2's DHT
    // block calls BatchPublisher::stage()/flush() on its very first loop
    // iteration (its `lastDHT` timer starts at 0, so it fires immediately),
    // which reliably lands before that 3s delay elapses. Taking an
    // uninitialized (null) FreeRTOS semaphore hits the exact same assert
    // as a null queue — semaphores are queues internally — i.e.
    // "assert failed: xQueueSemaphoreTake queue.c:1709 (( pxQueue ))".
    // Creating the mutex here, before any task starts, removes the race.
    // BatchPublisher::init() is idempotent — safe to also still run
    // unchanged inside NetworkTaskV2.
    BatchPublisher::init();

    // ── 7. Hardware WDT ───────────────────────────────────────
    TaskManager::initWatchdog();

    // ── 8. Provisioning (blocking before tasks) ───────────────
    if (BootManager::mode() == BootMode::PROVISIONING) {
        runProvisioningFlow();
        FlashWearGuard::flushAll();
        delay(200);
        esp_restart();
    }

    // ── 9. Safe mode — network + OTA tasks only ───────────────
    if (BootManager::mode() == BootMode::SAFE) {
        LOG_W("Main", "SAFE MODE — restricted task set");
        FaultManager::record(FaultCode::WATCHDOG_NEAR, "boot_safe", FaultSeverity::SEV_HIGH);
        // OPP-5: publish heartbeat every HEARTBEAT_MS so backend knows device is alive in safe mode.
        xTaskCreatePinnedToCore([](void*) {
            for (;;) {
                if (EventBus::isMqttConnected()) TelemetryPipeline::publishHeartbeat();
                vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_MS));
            }
        }, "SafeHB", 3072, nullptr, 2, nullptr, 0);
    }

    // ── 10. Log boot reason for observability ─────────────────
    LOG_I("Main", "Reset: %d  Crashes: %d  Mode: %d",
          (int)BootManager::resetReason(),
          (int)BootManager::crashCount(),
          (int)BootManager::mode());

    // ── 11. Launch tasks ──────────────────────────────────────
    // WiFiRoaming::init() handled inside SensorTaskV2_P2 init block
    TaskManagerV2::startAll();
    LOG_I("Main", "All tasks launched — free heap: %u", ESP.getFreeHeap());
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}

void runProvisioningFlow() {
#if USE_RGB
    RGBHAL::init();
    RGBHAL::set(RGBHAL::YELLOW);
#endif

// ── Serial provisioning (dev/factory only) ────────────────────────────────
#if ENABLE_SERIAL_PROV
    SerialProvisioning::start();
    unsigned long t = millis();
    while (!SerialProvisioning::tick()) {
        if (millis() - t > SERIAL_PROV_TIMEOUT_MS) break;
        delay(50);
    }
    if (SerialProvisioning::isSuccess()) {
#if USE_RGB
        RGBHAL::set(RGBHAL::GREEN);
#endif
        return;
    }
#endif

// ── BLE provisioning ──────────────────────────────────────────────────────
#if ENABLE_BLE
    // shouldStart() checks the BLE-specific fail counter AND heap — it does
    // NOT look at BootManager::crashCount(), so system crashes never disable BLE.
    if (BLEProvisioning::shouldStart()) {
        LOG_I("Main", "Starting BLE provisioning — heap: %u bytes free", ESP.getFreeHeap());

        bool bleInitOk = BLEProvisioning::start(Identity::get());

        if (bleInitOk) {
            // Drive the BLE window — tick() returns true on success or timeout
            while (!BLEProvisioning::tick()) {
                delay(100);
            }
            BLEProvisioning::stop();

            if (BLEProvisioning::isSuccess()) {
#if USE_RGB
                RGBHAL::set(RGBHAL::GREEN);
#endif
                LOG_I("Main", "BLE provisioning complete — heap: %u bytes free", ESP.getFreeHeap());
                return;
            }
            // Timed out — fall through to AP
            LOG_W("Main", "BLE timed out — falling back to AP provisioning");
        } else {
            // BLEProvisioning::start() returned false — init failed gracefully.
            // BLE-specific fail counter already incremented inside start().
            // Do NOT call BootManager::incrementCrashCount() here.
            LOG_W("Main", "BLE init failed — falling back to AP provisioning");
        }
    } else {
        LOG_W("Main", "BLE pre-flight failed — skipping to AP provisioning");
    }
#endif

// ── AP provisioning fallback ──────────────────────────────────────────────
#if USE_RGB
    RGBHAL::set(RGBHAL::BLUE);
#endif
    APProvisioning::start();
    // AP provisioning has no timeout in APProvisioning::tick() — add one here.
    // After AP_PROV_MAX_MS (15 min) with no success, reboot so the device
    // re-enters the provisioning flow on the next boot rather than blocking forever.
    {
        unsigned long apStart = millis();
        constexpr unsigned long AP_PROV_MAX_MS = 15UL * 60UL * 1000UL;
        while (!APProvisioning::tick()) {
            if (millis() - apStart > AP_PROV_MAX_MS) {
                LOG_W("Main", "AP provisioning timed out — rebooting");
                APProvisioning::stop();
                delay(300);
                esp_restart();
            }
            delay(100);
        }
    }
    APProvisioning::stop();
#if USE_RGB
    if (APProvisioning::isSuccess()) RGBHAL::set(RGBHAL::GREEN);
    else                             RGBHAL::set(RGBHAL::RED);
#endif
}
