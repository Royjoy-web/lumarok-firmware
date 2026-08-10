#pragma once
#include <ArduinoJson.h>
#include "../core/Types.h"
#include "../core/EventBus.h"
#include "../mqtt/MQTTTopics.h"
#include "../security/HMACVerifier.h"
#include "../security/CredentialStore.h"
#include "../telemetry/TimeSync.h"

// ─────────────────────────────────────────────────────────────
// LocalTokenProvisioner — Phase 1 hardening: pairing.
//
// Issues/rotates local_token, the credential LocalCommandServer verifies
// LAN commands against. Deliberately its own file with its own MQTT
// subscription, separate from CommandDispatcher.h — that file (the
// cloud device/system/OTA command path) is not touched by this change.
//
// Trust chain: the backend only sends a rotation over this topic after
// an authenticated app user completes pairing (POST /units/:id/pair-local,
// membership-checked same as emergency-stop). The message itself is signed
// with dev_secret — the one credential the unit already trusts out of the
// box — so a local_token can only ever be *installed* by someone who also
// controls the cloud path. Once installed, local_token is what LAN clients
// use instead, so the cloud secret itself never has to leave the backend.
// ─────────────────────────────────────────────────────────────

class LocalTokenProvisioner {
public:
    // Call once during MQTT post-connect init, alongside
    // CommandDispatcher::registerSubscriptions() — see NetworkTaskV2.h.
    static void registerSubscriptions() {
        MQTTTransport::subscribe(MQTTTopics::pairingSubscribe(), _onPairing, 1);
    }

private:
    // Payload: {"token": "<64-hex>", "ts": <ms>, "sig": "<hmac>"}
    // Canonical message signed by backend: "token:ts" — deliberately NOT
    // reusing buildCredRotateMessage/buildOTAMessage's shape so a captured
    // pairing message can't be replayed against another command class.
    static void _onPairing(const char* topic, const uint8_t* raw, unsigned int len) {
        StaticJsonDocument<256> doc;
        if (deserializeJson(doc, raw, len) != DeserializationError::Ok) return;

        const char* token = doc["token"] | "";
        const char* sig   = doc["sig"]   | "";
        long        ts    = doc["ts"]    | 0L;

        if (strlen(token) < 16 || strlen(token) > 64) {
            LOG_W("Pairing", "Rejected: invalid token length");
            return;
        }

        if (!_validateTimestamp(ts)) return;

        String msg = String(token) + ":" + String(ts);
        if (!HMACVerifier::verify(msg, CredentialStore::devSecret(), sig)) {
            LOG_W("Pairing", "Rejected: HMAC invalid");
            return;
        }

        Event e{};
        e.type = EventType::LOCAL_TOKEN_ROTATE_COMMAND;
        strlcpy(e.data.local_token.token, token, sizeof(e.data.local_token.token));
        strlcpy(e.data.local_token.sig,   sig,   sizeof(e.data.local_token.sig));
        e.data.local_token.ts = ts;

        EventBus::postCommand(e);
        LOG_I("Pairing", "local_token rotation accepted");
    }

    // Same 5-min replay window as CommandDispatcher, duplicated rather than
    // shared so this file has zero dependency on CommandDispatcher.h.
    static bool _validateTimestamp(long ts) {
        time_t now = TimeSync::bestEffort();
        if (now == 0) {
            LOG_W("Pairing", "Rejected — NTP not yet synced (ts=%ld)", ts);
            return false;
        }
        long diff = labs((long)now - ts);
        if (diff > 300) {
            LOG_W("Pairing", "Timestamp replay rejected: diff=%ld s", diff);
            return false;
        }
        return true;
    }
};
