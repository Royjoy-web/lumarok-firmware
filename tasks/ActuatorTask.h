#pragma once
#include "../core/TaskManager.h"
#include "../core/EventBus.h"
#include "../diagnostics/WatchdogManager.h"
#include "../hal/RelayHAL.h"
#include "../hal/ServoHAL.h"
#include "../hal/StepperHAL.h"
#include "../automation/DeviceRegistry.h"
#include "../telemetry/TelemetryPipeline.h"
#include "../provisioning/ProvisioningManager.h"
#include "../storage/NVSStore.h"
#include "../security/CredentialStore.h"

// _applyRelay — atomically updates registry state under lock,
// then performs GPIO write and telemetry publish OUTSIDE the lock.
//
// FIX v3.2: Uses DeviceRegistry::updateByName() which holds the lock
// for the entire multi-field write.  Previously lock() and unlock()
// wrapped only three fields, but findByName() was called WITHOUT the
// lock (possible null-deref if updateDeviceId() ran concurrently).
static void _applyRelay(const char* room, const char* name, bool on) {
    uint8_t gpio       = 0;
    bool    newState   = on;
    bool    found      = false;

    // Atomic update of all three time-correlated fields under one lock
    found = DeviceRegistry::updateByName(room, name,
        [on, &gpio](DeviceState& d) {
            d.power_state    = on;
            d.last_toggle_ms = millis();
            d.on_since_ms    = on ? millis() : 0;
            gpio             = d.gpio;
        });

    if (!found) {
        LOG_W("Actuator", "Device not found for relay apply: %s/%s", room, name);
        return;
    }

    // GPIO write and publish happen outside the lock — no blocking under it
    RelayHAL::set(gpio, on);

    // OPP-7: publish relay state change — route goes EventBus::post() → postAlert() → MQTTTxTask
    {
        Event re{};
        re.type = EventType::RELAY_STATE_CHANGED;
        strlcpy(re.data.relay.device_id, name,      sizeof(re.data.relay.device_id));
        re.data.relay.gpio  = gpio;
        re.data.relay.state = on;
        strlcpy(re.data.relay.source, "actuator", sizeof(re.data.relay.source));
        EventBus::postAlert(re);
    }

    // Snapshot for NVS and telemetry (no lock needed — just reading locals)
    DeviceState snap;
    if (DeviceRegistry::findByName(room, name, snap)) {
        DeviceRegistry::persistDeviceState(snap);
        TelemetryPipeline::publishDeviceState(snap);
    }
    LOG_I("Actuator", "%s/%s → %s", room, name, on ? "ON" : "OFF");
}

void actuatorTaskFn(void* pvParam) {
    // FIX v3.2: Register with WatchdogManager (not just hardware WDT) so that
    // stack watermarks appear in diagnostics JSON and soft-watchdog monitoring
    // catches this task if it stalls on EventBus::waitCommand.
    int8_t wdIdx = WatchdogManager::registerTask("ActuatorTask", 10000);

    // Init relay GPIOs from registry snapshot — safe at boot before other tasks write
    int devCount = DeviceRegistry::deviceCount();
    for (int i = 0; i < devCount; i++) {
        DeviceState snap;
        if (DeviceRegistry::getDevice(i, snap) && snap.type == DeviceType::RELAY) {
            RelayHAL::init(snap.gpio);
            RelayHAL::set(snap.gpio, snap.power_state);
        }
    }
    ServoHAL::init();
    StepperHAL::init();

    LOG_I("ActuatorTask", "Running on core %d", (int)xPortGetCoreID());

    for (;;) {
        WatchdogManager::checkin(wdIdx);

        Event e{};
        if (!EventBus::waitCommand(e, pdMS_TO_TICKS(500))) continue;

        if (e.type == EventType::RELAY_COMMAND || e.type == EventType::COMMAND_RECEIVED) {

            // ── Stepper (gate) ────────────────────────────────
            // Check device type before acting — need a snapshot
            DeviceState snap;
            if (!DeviceRegistry::findByName(e.data.command.room, e.data.command.device_name, snap)) {
                LOG_W("Actuator", "Device not found: %s/%s",
                      e.data.command.room, e.data.command.device_name);
                continue;
            }

            if (snap.type == DeviceType::STEPPER) {
                if (e.data.command.action == CommandAction::ON)
                    StepperHAL::moveTo(STEPPER_STEPS);
                else if (e.data.command.action == CommandAction::OFF)
                    StepperHAL::moveTo(0);
                else if (e.data.command.action == CommandAction::TOGGLE)
                    StepperHAL::moveTo(StepperHAL::currentPos() > 0 ? 0 : STEPPER_STEPS);
                continue;
            }

            if (snap.type == DeviceType::SERVO) {
                int pos = (e.data.command.value >= 0) ? e.data.command.value
                        : (e.data.command.action == CommandAction::ON ? 180 : 0);
                ServoHAL::setPosition(pos);
                // Atomic position update
                DeviceRegistry::updateByName(snap.room, snap.name,
                    [pos](DeviceState& d){ d.position = pos; });
                DeviceState updated;
                if (DeviceRegistry::findByName(snap.room, snap.name, updated))
                    TelemetryPipeline::publishDeviceState(updated);
                continue;
            }

            // ── Relay ─────────────────────────────────────────
            bool newState;
            switch (e.data.command.action) {
                case CommandAction::ON:     newState = true;             break;
                case CommandAction::OFF:    newState = false;            break;
                case CommandAction::TOGGLE: newState = !snap.power_state; break;
                default: continue;
            }
            _applyRelay(e.data.command.room, e.data.command.device_name, newState);

        } else if (e.type == EventType::CRED_ROTATE_COMMAND) {
            ProvisioningManager::rotateMQTTCredentials(
                e.data.cred_rotate.mqtt_user,
                e.data.cred_rotate.mqtt_pass,
                e.data.cred_rotate.dev_secret);
            MQTTTransport::disconnect();

        } else if (e.type == EventType::LOCAL_TOKEN_ROTATE_COMMAND) {
            // HMAC already verified in LocalTokenProvisioner before this was
            // posted — same trust boundary as CRED_ROTATE_COMMAND above.
            // No MQTT disconnect needed: local_token never touches the
            // cloud MQTT session, only LocalCommandServer's HMAC check.
            CredentialStore::storeLocalToken(e.data.local_token.token);
            LOG_I("Actuator", "local_token stored — unit now paired for LAN control");

        } else if (e.type == EventType::SYSTEM_RESTART) {
            LOG_I("Actuator", "Restart command received");
            DeviceRegistry::persistAllStates();   // snapshot-based, no long lock
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
    }
}
