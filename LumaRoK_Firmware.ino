// ═══════════════════════════════════════════════════════════════
//  LumaRoK C10 — Enterprise Hardened Firmware v3.2.0
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
#include "mqtt/MQTTTopics.h"
#include "mqtt/MQTTTransport.h"
#include "mqtt/CommandDispatcher.h"

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
#include "tasks/SensorTaskV2.h"
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
    LOG_I("Main", "LumaRoK v3.2.0 (hardened)  HW:%s", HARDWARE_REVISION);

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
