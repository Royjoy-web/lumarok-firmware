#pragma once
#include "../storage/NVSStore.h"
#include "../core/Config.h"

// CredentialStore — the ONLY place secrets are read from or written to.
// No other module may read mqtt_user, mqtt_pass, dev_secret directly.
class CredentialStore {
public:
    // ── Read ──────────────────────────────────────────────────
    static String mqttUser()     { return NVSStore::getString(NVS_NS_SECRETS, NVS_KEY_MQTT_USER); }
    static String mqttPass()     { return NVSStore::getString(NVS_NS_SECRETS, NVS_KEY_MQTT_PASS); }
    static String devSecret()    { return NVSStore::getString(NVS_NS_SECRETS, NVS_KEY_DEV_SECRET); }
    // Phase 1 hardening: local_token is a separate LAN-only credential, issued via
    // pairing (LocalTokenProvisioner) and never used on the cloud MQTT path. This
    // means a phone on the LAN never needs to know dev_secret, so compromising the
    // local HTTP listener no longer exposes the credential that also guards OTA
    // and cloud device commands.
    static String localToken()   { return NVSStore::getString(NVS_NS_SECRETS, NVS_KEY_LOCAL_TOKEN); }
    static String wifiSSID()     { return NVSStore::getString(NVS_NS_CONFIG,  NVS_KEY_WIFI_SSID); }
    static String wifiPass()     { return NVSStore::getString(NVS_NS_CONFIG,  NVS_KEY_WIFI_PASS); }
    static String backendURL()   { return NVSStore::getString(NVS_NS_CONFIG,  NVS_KEY_BACKEND_URL); }
    static String apPassword()   {
        String p = NVSStore::getString(NVS_NS_CONFIG, NVS_KEY_AP_PASS);
        return p.length() ? p : AP_PASSWORD_DEFAULT;
    }

    // ── Write (provisioning only) ─────────────────────────────
    static bool storeWiFi(const String& ssid, const String& pass) {
        bool ok = NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_WIFI_SSID, ssid);
        ok     &= NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_WIFI_PASS, pass);
        return ok;
    }

    static bool storeMQTTCredentials(const String& user, const String& pass) {
        bool ok = NVSStore::putString(NVS_NS_SECRETS, NVS_KEY_MQTT_USER, user);
        ok     &= NVSStore::putString(NVS_NS_SECRETS, NVS_KEY_MQTT_PASS, pass);
        return ok;
    }

    static bool storeDevSecret(const String& secret) {
        return NVSStore::putString(NVS_NS_SECRETS, NVS_KEY_DEV_SECRET, secret);
    }

    static bool storeLocalToken(const String& token) {
        return NVSStore::putString(NVS_NS_SECRETS, NVS_KEY_LOCAL_TOKEN, token);
    }

    static bool storeBackendURL(const String& url) {
        return NVSStore::putString(NVS_NS_CONFIG, NVS_KEY_BACKEND_URL, url);
    }

    // ── Validation ────────────────────────────────────────────
    static bool hasWiFiCredentials() {
        return NVSStore::exists(NVS_NS_CONFIG,  NVS_KEY_WIFI_SSID) &&
               NVSStore::exists(NVS_NS_SECRETS, NVS_KEY_MQTT_USER);
    }

    static bool hasDevSecret() {
        String s = devSecret();
        return s.length() >= 16;
    }

    // Unpaired units simply have no local_token yet — LocalCommandServer
    // rejects local commands until pairing runs, cloud path unaffected.
    static bool hasLocalToken() {
        String s = localToken();
        return s.length() >= 16;
    }
};
