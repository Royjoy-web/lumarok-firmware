#pragma once
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "../core/Types.h"
#include "../core/EventBus.h"
#include "../core/Identity.h"
#include "../core/Config.h"
#include "../security/HMACVerifier.h"
#include "../security/CredentialStore.h"

// ─────────────────────────────────────────────────────────────
// LocalCommandServer — Phase 1 local control path.
//
// Lets the phone app control devices directly over the LAN, without
// depending on internet reachability or the cloud MQTT broker.
//
// This runs ALONGSIDE the existing MQTTTransport / CommandDispatcher
// path — it does not replace it, and the cloud path is untouched.
// A locally-issued command reuses the exact same HMAC verification
// mechanics (HMACVerifier::buildDeviceCommandMessage) but is signed
// with local_token, not dev_secret — a separate LAN-only credential
// issued via pairing (LocalTokenProvisioner.h). This keeps dev_secret,
// which also guards OTA and cloud device commands, off the phone/LAN
// attack surface entirely: compromising a local command listener no
// longer exposes the credential the cloud path depends on. A unit that
// hasn't been paired yet simply has no local_token (CredentialStore::
// hasLocalToken() == false) and rejects local commands until it does;
// the cloud path is completely unaffected either way. Verified commands
// are posted through the same EventBus::postCommand() queue that
// CommandDispatcher uses for MQTT commands, so it flows through the
// identical ActuatorTask -> RELAY_STATE_CHANGED -> TelemetryPipeline
// pipeline. When MQTT/internet is down, that pipeline's existing
// offline buffering (PriorityBuffer / BatchPublisher::drainOffline)
// already queues the resulting state-change telemetry for sync once
// connectivity returns — no new reconciliation logic was needed here.
//
// Only starts once WiFi (LAN) is up — independent of MQTT/cloud
// state — so it keeps working through cloud outages, which is the
// whole point. Advertises via mDNS as <unit-id>.local so the app can
// discover the unit on the LAN without any cloud lookup.
// ─────────────────────────────────────────────────────────────

#define LOCAL_CMD_PORT          3282     // distinct from AP_PORT (80) and MQTT_PORT (8883)
#define LOCAL_CMD_TS_WINDOW_MS  300000UL // 5 min — matches CommandDispatcher's device-command window

class LocalCommandServer {
public:
    // Call once WiFi comes up (ReconnectEngine::wifiConnected() == true).
    // Safe to call again after a WiFi drop + reconnect; it no-ops if
    // already running.
    static void begin() {
        if (_started) return;

        _server = new WebServer(LOCAL_CMD_PORT);
        _server->on("/command", HTTP_POST, _handleCommand);
        _server->on("/status",  HTTP_GET,  _handleStatus);
        _server->onNotFound(_handleNotFound);
        _server->begin();

        String unitId = Identity::get();
        if (MDNS.begin(unitId.c_str())) {
            MDNS.addService("lumarok", "tcp", LOCAL_CMD_PORT);
            MDNS.addServiceTxt("lumarok", "tcp", "unit_id", unitId);
            LOG_I("LocalCmd", "mDNS advertised as %s.local", unitId.c_str());
        } else {
            LOG_W("LocalCmd", "mDNS init failed — local discovery unavailable, direct IP still works");
        }

        _started = true;
        LOG_I("LocalCmd", "Local command server listening on port %d", LOCAL_CMD_PORT);
    }

    // Call every tick from NetworkTaskV2 whenever WiFi is up. Deliberately
    // NOT gated on MQTT state — must keep serving through cloud outages.
    static void tick() {
        if (!_started) return;
        _server->handleClient();
    }

    // Call on WiFi loss so a stale server/mDNS instance isn't left running
    // across an SSID/IP change; begin() will re-init cleanly on reconnect.
    static void end() {
        if (!_started) return;
        MDNS.end();
        _server->stop();
        delete _server;
        _server = nullptr;
        _started = false;
        LOG_I("LocalCmd", "Local command server stopped (WiFi down)");
    }

    static bool isRunning() { return _started; }

private:
    static WebServer* _server;
    static bool        _started;

    // Mirrors CommandDispatcher's private timestamp check — duplicated
    // rather than exposed from CommandDispatcher so that file (the
    // MQTT/cloud command path) stays completely untouched by this change.
    static bool _validateTimestamp(long ts) {
        if (ts <= 0) return false;
        long now  = (long)time(nullptr);
        long diff = now - ts;
        if (diff < 0) diff = -diff;
        return (unsigned long)diff * 1000UL <= LOCAL_CMD_TS_WINDOW_MS;
    }

    static void _handleStatus() {
        StaticJsonDocument<192> doc;
        doc["unit_id"]       = Identity::get();
        doc["uptime_ms"]     = millis();
        doc["rssi"]          = WiFi.RSSI();
        doc["local_control"] = true;
        doc["paired"]        = CredentialStore::hasLocalToken();
        String out;
        serializeJson(doc, out);
        _server->send(200, "application/json", out);
    }

    static void _handleNotFound() {
        _server->send(404, "application/json", "{\"error\":\"not_found\"}");
    }

    // Same JSON shape and canonical message as the MQTT device-command
    // path in CommandDispatcher::_onDeviceCommand — "action:room:device:ts"
    // — but signed with local_token, not dev_secret. The app switches to
    // signing with local_token once pairing has handed it one; until then
    // there's nothing valid to sign with and commands are rejected below.
    static void _handleCommand() {
        if (!CredentialStore::hasLocalToken()) {
            _server->send(403, "application/json", "{\"error\":\"not_paired\"}");
            return;
        }

        if (!_server->hasArg("plain")) {
            _server->send(400, "application/json", "{\"error\":\"missing_body\"}");
            return;
        }

        StaticJsonDocument<384> doc;
        if (deserializeJson(doc, _server->arg("plain")) != DeserializationError::Ok) {
            _server->send(400, "application/json", "{\"error\":\"bad_json\"}");
            return;
        }

        const char* room   = doc["room"]   | "";
        const char* device = doc["device"] | "";
        const char* action = doc["action"] | "";
        int         value  = doc["value"]  | -1;
        const char* sig    = doc["sig"]    | "";
        long        ts     = doc["ts"]     | 0L;

        if (!room[0] || !device[0] || !action[0]) {
            _server->send(400, "application/json", "{\"error\":\"missing_fields\"}");
            return;
        }

        if (!_validateTimestamp(ts)) {
            LOG_W("LocalCmd", "Rejected: stale/invalid timestamp");
            _server->send(401, "application/json", "{\"error\":\"stale_timestamp\"}");
            return;
        }

        String msg = HMACVerifier::buildDeviceCommandMessage(action, room, device, ts);
        if (!HMACVerifier::verify(msg, CredentialStore::localToken(), sig)) {
            LOG_W("LocalCmd", "Rejected: HMAC invalid (%s/%s action=%s)", room, device, action);
            _server->send(401, "application/json", "{\"error\":\"invalid_signature\"}");
            return;
        }

        CommandAction ca;
        if      (strcmp(action, "on")     == 0) ca = CommandAction::ON;
        else if (strcmp(action, "off")    == 0) ca = CommandAction::OFF;
        else if (strcmp(action, "toggle") == 0) ca = CommandAction::TOGGLE;
        else if (strcmp(action, "set")    == 0) ca = CommandAction::SET_VALUE;
        else {
            _server->send(400, "application/json", "{\"error\":\"unknown_action\"}");
            return;
        }

        Event e{};
        e.type = EventType::COMMAND_RECEIVED;
        strlcpy(e.data.command.room,        room,   sizeof(e.data.command.room));
        strlcpy(e.data.command.device_name, device, sizeof(e.data.command.device_name));
        e.data.command.action = ca;
        e.data.command.value  = value;

        // Same queue CommandDispatcher posts to for MQTT commands — from
        // here on, local and cloud commands are indistinguishable to the
        // rest of the firmware.
        if (!EventBus::postCommand(e)) {
            _server->send(503, "application/json", "{\"error\":\"command_queue_full\"}");
            return;
        }

        LOG_I("LocalCmd", "Local command accepted: %s/%s -> %s", room, device, action);
        _server->send(200, "application/json", "{\"ok\":true}");
    }
};

WebServer* LocalCommandServer::_server  = nullptr;
bool       LocalCommandServer::_started = false;
