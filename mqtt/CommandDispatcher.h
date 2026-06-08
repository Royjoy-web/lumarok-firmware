#pragma once
#include <ArduinoJson.h>
#include "../core/Types.h"
#include "../core/EventBus.h"
#include "../mqtt/MQTTTopics.h"
#include "../automation/DeviceRegistry.h"
#include "../security/HMACVerifier.h"
#include "../security/CredentialStore.h"
#include "../telemetry/TimeSync.h"

class CommandDispatcher {
public:
    // Called during MQTTTransport::subscribe registration
    static void registerSubscriptions() {
        MQTTTransport::subscribe(MQTTTopics::cmdSubscribe(), _onDeviceCommand, 1);
        MQTTTransport::subscribe(MQTTTopics::sysSubscribe(), _onSystemCommand, 1);
        MQTTTransport::subscribe(MQTTTopics::otaSubscribe(), _onOTACommand,    1);
    }

private:
    // ── Device command: lumarok/{id}/{room}/{device}/command ──
    static void _onDeviceCommand(const char* topic, const uint8_t* raw, unsigned int len) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, raw, len) != DeserializationError::Ok) return;

        char room[24] = {}, devName[32] = {};
        if (!MQTTTopics::parseCommandTopic(topic, room, sizeof(room),
                                            devName, sizeof(devName))) {
            LOG_W("Cmd", "Could not parse topic: %s", topic);
            return;
        }

        const char* action = doc["action"] | "";
        int         value  = doc["value"]  | -1;

        CommandAction ca;
        if      (strcmp(action, "on")     == 0) ca = CommandAction::ON;
        else if (strcmp(action, "off")    == 0) ca = CommandAction::OFF;
        else if (strcmp(action, "toggle") == 0) ca = CommandAction::TOGGLE;
        else if (strcmp(action, "set")    == 0) ca = CommandAction::SET_VALUE;
        else { LOG_W("Cmd", "Unknown action: %s", action); return; }

        Event e{};
        e.type = EventType::COMMAND_RECEIVED;
        strlcpy(e.data.command.room,        room,    sizeof(e.data.command.room));
        strlcpy(e.data.command.device_name, devName, sizeof(e.data.command.device_name));
        e.data.command.action = ca;
        e.data.command.value  = value;

        EventBus::postCommand(e);
        LOG_D("Cmd", "Device command: %s/%s → %s", room, devName, action);
    }

    // ── System command ────────────────────────────────────────
    static void _onSystemCommand(const char* topic, const uint8_t* raw, unsigned int len) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, raw, len) != DeserializationError::Ok) return;

        const char* cmd = doc["command"] | "";

        if (strcmp(cmd, "restart") == 0) {
            LOG_I("Cmd", "Remote restart requested");
            Event e{}; e.type = EventType::SYSTEM_RESTART;
            EventBus::postCommand(e);
            return;
        }

        if (strcmp(cmd, "rotate_creds") == 0) {
            _handleCredRotate(doc);
            return;
        }

        LOG_W("Cmd", "Unknown system command: %s", cmd);
    }

    static void _handleCredRotate(JsonDocument& doc) {
        const char* mqttUser  = doc["mqtt_user"]   | "";
        const char* mqttPass  = doc["mqtt_pass"]   | "";
        const char* devSecret = doc["dev_secret"]  | "";
        const char* sig       = doc["sig"]         | "";
        long        ts        = doc["ts"]          | 0L;

        if (!_validateTimestamp(ts)) return;

        String msg = HMACVerifier::buildCredRotateMessage(mqttUser, mqttPass, devSecret, ts);
        if (!HMACVerifier::verify(msg, CredentialStore::devSecret(), sig)) {
            LOG_W("Cmd", "Credential rotation — HMAC invalid");
            return;
        }

        Event e{}; e.type = EventType::CRED_ROTATE_COMMAND;
        strlcpy(e.data.cred_rotate.mqtt_user,  mqttUser,  sizeof(e.data.cred_rotate.mqtt_user));
        strlcpy(e.data.cred_rotate.mqtt_pass,  mqttPass,  sizeof(e.data.cred_rotate.mqtt_pass));
        strlcpy(e.data.cred_rotate.dev_secret, devSecret, sizeof(e.data.cred_rotate.dev_secret));
        strlcpy(e.data.cred_rotate.sig,        sig,       sizeof(e.data.cred_rotate.sig));
        e.data.cred_rotate.ts = ts;
        EventBus::postCommand(e);
        LOG_I("Cmd", "Credential rotation accepted");
    }

    // ── OTA command ───────────────────────────────────────────
    static void _onOTACommand(const char* topic, const uint8_t* raw, unsigned int len) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, raw, len) != DeserializationError::Ok) return;

        const char* url     = doc["url"]     | "";
        const char* version = doc["version"] | "";
        const char* sha256  = doc["sha256"]  | "";
        const char* sig     = doc["sig"]     | "";
        long        ts      = doc["ts"]      | 0L;

        if (strlen(url) < 8 || strlen(sha256) != 64) {
            LOG_W("Cmd", "OTA: invalid url or sha256");
            return;
        }

        if (!_validateTimestamp(ts)) return;

        String msg = HMACVerifier::buildOTAMessage(url, version, sha256, ts);
        if (!HMACVerifier::verify(msg, CredentialStore::devSecret(), sig)) {
            LOG_W("Cmd", "OTA: HMAC verification failed");
            return;
        }

        Event e{}; e.type = EventType::OTA_COMMAND;
        strlcpy(e.data.ota.url,     url,     sizeof(e.data.ota.url));
        strlcpy(e.data.ota.version, version, sizeof(e.data.ota.version));
        strlcpy(e.data.ota.sha256,  sha256,  sizeof(e.data.ota.sha256));
        strlcpy(e.data.ota.sig,     sig,     sizeof(e.data.ota.sig));
        e.data.ota.ts = ts;
        EventBus::postOTA(e);
        LOG_I("Cmd", "OTA accepted: version %s", version);
    }

    // ── Timestamp replay guard (requires SNTP sync) ───────────
    static bool _validateTimestamp(long ts) {
        time_t now = TimeSync::bestEffort();
        if (now == 0) {
            // Reject commands before NTP sync to prevent replay attacks at boot
            LOG_W("Cmd", "Command rejected — NTP not yet synced (ts=%ld)", ts);
            return false;
        }
        long diff = labs((long)now - ts);
        if (diff > 300) {   // 5-minute window
            LOG_W("Cmd", "Timestamp replay rejected: diff=%ld s", diff);
            return false;
        }
        return true;
    }
};
