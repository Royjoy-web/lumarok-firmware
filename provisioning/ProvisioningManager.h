#pragma once
#include "../core/Types.h"
#include "../core/Config.h"
#include "../storage/NVSStore.h"
#include "../security/CredentialStore.h"

// Forward declarations
class BLEProvisioning;
class APProvisioning;
class SerialProvisioning;

enum class ProvMode : uint8_t { NONE, BLE, AP, SERIAL_PROV };

class ProvisioningManager {
public:
    static void init() {
        _locked = NVSStore::getBool(NVS_NS_CONFIG, NVS_KEY_PROV_LOCKED, false);
        LOG_I("Prov", "Provisioning %s", _locked ? "LOCKED" : "OPEN");
    }

    // ── Lock control ──────────────────────────────────────────
    static void lock() {
        _locked = true;
        NVSStore::putBool(NVS_NS_CONFIG, NVS_KEY_PROV_LOCKED, true);
        LOG_I("Prov", "Provisioning locked");
    }

    static void unlock() {
        _locked = false;
        NVSStore::putBool(NVS_NS_CONFIG, NVS_KEY_PROV_LOCKED, false);
        LOG_W("Prov", "Provisioning unlocked");
    }

    static bool isLocked() { return _locked; }

    // ── Credential commit (shared by all provisioning paths) ──
    // Returns false if locked (all paths call this; all fail if locked)
    static bool commitCredentials(const String& ssid, const String& pass,
                                   const String& mqttUser, const String& mqttPass,
                                   const String& devSecret, const String& backendUrl) {
        if (_locked) {
            LOG_W("Prov", "Commit rejected — provisioning is locked");
            return false;
        }
        bool ok = CredentialStore::storeWiFi(ssid, pass);
        ok &= CredentialStore::storeMQTTCredentials(mqttUser, mqttPass);
        ok &= CredentialStore::storeDevSecret(devSecret);
        ok &= CredentialStore::storeBackendURL(backendUrl);
        if (ok) {
            LOG_I("Prov", "Credentials committed — SSID: %s", ssid.c_str());
            lock();   // Lock after first successful provisioning
        }
        return ok;
    }

    // ── Credential rotation (authenticated — does NOT go through lock) ──
    static bool rotateMQTTCredentials(const String& user, const String& pass,
                                       const String& secret) {
        bool ok = CredentialStore::storeMQTTCredentials(user, pass);
        ok &= CredentialStore::storeDevSecret(secret);
        LOG_I("Prov", "Credential rotation %s", ok ? "OK" : "FAILED");
        return ok;
    }

    static ProvMode activeMode() { return _activeMode; }
    static void setActiveMode(ProvMode m) { _activeMode = m; }

private:
    static bool     _locked;
    static ProvMode _activeMode;
};

inline bool     ProvisioningManager::_locked     = false;
inline ProvMode ProvisioningManager::_activeMode = ProvMode::NONE;
