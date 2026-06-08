// ═══════════════════════════════════════════════════════════════
//  LumaRoK C10 — Enterprise Firmware v3.0.0
//  Main entry point: setup() and loop() only.
//  All business logic lives in RTOS tasks.
// ═══════════════════════════════════════════════════════════════

// ── Core ──────────────────────────────────────────────────────
#include "core/Types.h"
#include "core/Config.h"
#include "core/Identity.h"
#include "core/EventBus.h"
#include "core/TaskManager.h"
#include "core/BootManager.h"

// ── Storage & Security ────────────────────────────────────────
#include "storage/NVSStore.h"
#include "storage/TelemetryBuffer.h"
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
#include "mqtt/MQTTTopics.h"
#include "mqtt/MQTTTransport.h"
#include "mqtt/CommandDispatcher.h"

// ── Telemetry ─────────────────────────────────────────────────
#include "telemetry/TimeSync.h"
#include "telemetry/TelemetryPipeline.h"

// ── Automation & Safety ───────────────────────────────────────
#include "automation/DeviceRegistry.h"
#include "automation/SafetyService.h"
#include "automation/AutomationService.h"

// ── OTA ───────────────────────────────────────────────────────
#include "ota/OTAVerifier.h"
#include "ota/RollbackManager.h"
#include "ota/OTAManager.h"

// ── Provisioning ──────────────────────────────────────────────
#include "provisioning/ProvisioningManager.h"
#include "provisioning/BLEProvisioning.h"
#include "provisioning/APProvisioning.h"
#include "provisioning/SerialProvisioning.h"

// ── Diagnostics ───────────────────────────────────────────────
#include "diagnostics/CrashLogger.h"
#include "diagnostics/DiagnosticsManager.h"

// ── Tasks (each file defines one task function) ───────────────
#include "tasks/SafetyTask.h"
#include "tasks/SensorTask.h"
#include "tasks/NetworkTask.h"
#include "tasks/ActuatorTask.h"
#include "tasks/MQTTTasks.h"
#include "tasks/OTAAndDiagTasks.h"

// ─────────────────────────────────────────────────────────────
void runProvisioningFlow();

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(200);
    LOG_I("Main", "LumaRoK Firmware v%s  HW: %s", FIRMWARE_VERSION, HARDWARE_REVISION);

    // ── 1. Identity (before WiFi, reads MAC) ──────────────────
    Identity::init();
    LOG_I("Main", "Unit ID: %s", Identity::get().c_str());

    // ── 2. Boot mode detection ────────────────────────────────
    BootManager::init();

    // ── 3. Rollback check — runs before anything else ─────────
    RollbackManager::checkAndRollback();

    // ── 4. Core primitives ────────────────────────────────────
    EventBus::init();
    TelemetryBuffer::init();
    DeviceRegistry::init();
    ProvisioningManager::init();

    // ── 5. Watchdog ───────────────────────────────────────────
    TaskManager::initWatchdog();

    // ── 6. Provisioning flow (blocks until credentials stored) ─
    if (BootManager::mode() == BootMode::PROVISIONING) {
        LOG_I("Main", "No credentials — entering provisioning flow");
        runProvisioningFlow();
        // After successful provisioning, restart cleanly
        LOG_I("Main", "Provisioning complete — restarting");
        delay(500);
        esp_restart();
    }

    // ── 7. Safe mode — await OTA only ─────────────────────────
    if (BootManager::mode() == BootMode::SAFE) {
        LOG_W("Main", "SAFE MODE — only OTA updates accepted");
        // TaskManager still starts NetworkTask + OTATask only
    }

    // ── 8. Launch RTOS tasks ──────────────────────────────────
    TaskManager::startAll();

    LOG_I("Main", "All tasks launched — setup() complete");
    // setup() returns; Arduino loop() becomes the idle task
}

void loop() {
    // Intentionally empty.
    // All work runs in FreeRTOS tasks pinned to Core 0 and Core 1.
    // This loop runs on Core 1 at the lowest idle priority.
    vTaskDelay(pdMS_TO_TICKS(10000));
}

// ─────────────────────────────────────────────────────────────
// Provisioning flow — blocking, runs before tasks start.
// Tries: Serial (factory) → BLE → AP portal in sequence.
// ─────────────────────────────────────────────────────────────
void runProvisioningFlow() {
    RGBHAL::init();
    RGBHAL::set(RGBHAL::YELLOW);

    // ── Serial (factory build only) ───────────────────────────
#if ENABLE_SERIAL_PROV
    SerialProvisioning::start();
    unsigned long t = millis();
    while (!SerialProvisioning::tick()) {
        if (millis() - t > SERIAL_PROV_TIMEOUT_MS) break;
        delay(50);
    }
    if (SerialProvisioning::isSuccess()) { RGBHAL::set(RGBHAL::GREEN); return; }
#endif

    // ── BLE ───────────────────────────────────────────────────
#if ENABLE_BLE
    BLEProvisioning::start(Identity::get());
    unsigned long bleStart = millis();
    while (!BLEProvisioning::tick()) {
        if (millis() - bleStart > BLE_TIMEOUT_MS) break;
        delay(100);
    }
    BLEProvisioning::stop();
    if (BLEProvisioning::isSuccess()) { RGBHAL::set(RGBHAL::GREEN); return; }
#endif

    // ── AP Captive Portal (fallback — runs until success) ─────
    RGBHAL::set(RGBHAL::BLUE);
    APProvisioning::start();
    while (!APProvisioning::isSuccess()) {
        APProvisioning::tick();
        delay(10);
    }
    APProvisioning::stop();
    RGBHAL::set(RGBHAL::GREEN);
}
