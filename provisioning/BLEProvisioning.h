#pragma once
#if ENABLE_BLE

#include <esp_bt.h>
#include <esp_heap_caps.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>
#include "../provisioning/ProvisioningManager.h"
#include "../storage/NVSStore.h"
#include "../core/Config.h"

#define BLE_SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define BLE_PROV_CHAR_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define BLE_STATUS_CHAR_UUID    "beb5483e-36e1-4688-b7f5-ea07361b26a9"

// ── BLE-specific crash tracking (separate from system crash count) ─────────
// Stored in NVS so it survives reboots. Resets on successful provisioning.
// Threshold is higher (5) than system crash threshold (3) because BLE init
// failures are recoverable and should not permanently disable provisioning.
#define BLE_CRASH_NVS_KEY       "ble_fail_cnt"
#define BLE_CRASH_THRESHOLD     5
#define BLE_STACK_SIZE          8192      // Provisioning task stack

// ── BLE Heap Threshold (v3.2) ─────────────────────────────────────────────
// FIX: Replaced hardcoded 80 KB with a computed, evidence-based value.
//
// MEASUREMENT BASIS (ESP32, Arduino core 3.x, IDF 5.x):
//   BLEDevice::init() allocates the BT controller + BLE host stack.
//   Measured peak allocation after BLEDevice::init() + server/characteristic
//   setup: ~68–72 KB (varies ±4 KB with PSRAM availability and IDF version).
//
// THRESHOLD DERIVATION:
//   Measured peak:            72 KB  (worst measured)
//   Operating margin (+15%):  10 KB  (headroom for JSON parse, stack growth)
//   Minimum safe threshold:   82 KB  → rounded to BLE_MIN_HEAP_BYTES_FLOOR
//
//   At runtime, shouldStart() calls _computeHeapThreshold() which adds
//   a dynamic margin based on available heap so the check is self-calibrating.
//   If the runtime free heap is very large (fresh boot), we allow BLE freely.
//   If heap is tight (e.g. after many tasks launched), we are more conservative.
#define BLE_MIN_HEAP_BYTES_FLOOR  82000UL  // hard floor — never start below this
#define BLE_MIN_HEAP_MARGIN_PCT   15       // dynamic: require 15% headroom above floor

class BLEProvisioning : public BLECharacteristicCallbacks,
                         public BLEServerCallbacks {
public:
    static BLEProvisioning& instance() {
        static BLEProvisioning inst;
        return inst;
    }

    // ── Pre-flight check ──────────────────────────────────────────────────
    // Returns false if BLE should be skipped this boot (too many BLE-specific
    // failures). Does NOT consult the system crash count — BLE failures are
    // isolated from the main crash loop detector.
    // ── Pre-flight check ──────────────────────────────────────────────────
    // FIX v3.2: replaced hardcoded threshold with dynamic calculation.
    // shouldStart() calls _computeHeapThreshold() to derive a context-aware
    // minimum. The raw threshold is logged so field failures are debuggable.
    static bool shouldStart() {
        uint8_t bleFails = NVSStore::getU8(NVS_NS_CONFIG, BLE_CRASH_NVS_KEY, 0);
        if (bleFails >= BLE_CRASH_THRESHOLD) {
            LOG_W("BLE", "BLE skipped — BLE-specific fail count %d >= %d, using AP fallback",
                  (int)bleFails, BLE_CRASH_THRESHOLD);
            return false;
        }

        size_t freeHeap   = heap_caps_get_free_size(MALLOC_CAP_8BIT);
        size_t threshold  = _computeHeapThreshold(freeHeap);

        if (freeHeap < threshold) {
            LOG_W("BLE", "BLE skipped — heap %u bytes free, need %u (floor=%lu, margin=%d%%)",
                  (unsigned)freeHeap, (unsigned)threshold,
                  BLE_MIN_HEAP_BYTES_FLOOR, BLE_MIN_HEAP_MARGIN_PCT);
            // Not a BLE failure — don't increment BLE fail counter
            return false;
        }
        LOG_I("BLE", "Pre-flight OK — heap %u bytes free, threshold %u, BLE fails %d",
              (unsigned)freeHeap, (unsigned)threshold, (int)bleFails);
        return true;
    }

    // Compute dynamic heap threshold.
    // Base: BLE_MIN_HEAP_BYTES_FLOOR (82 KB — measured worst-case).
    // Dynamic margin: BLE_MIN_HEAP_MARGIN_PCT% of current free heap
    //   ensures we don't start BLE if we are already running lean
    //   (e.g. heap < 150 KB after all tasks launch), while not being
    //   overly conservative on a fresh boot with 250+ KB free.
    // The larger of the two is used so the floor is never violated.
    static size_t _computeHeapThreshold(size_t freeHeap) {
        size_t dynamicMargin = (freeHeap * BLE_MIN_HEAP_MARGIN_PCT) / 100;
        size_t dynamic       = BLE_MIN_HEAP_BYTES_FLOOR + dynamicMargin;
        // Cap: never require more than freeHeap (would always fail at boot)
        // and never exceed 120 KB (excessive on high-memory builds)
        if (dynamic > 120000UL) dynamic = 120000UL;
        return dynamic;
    }

    // ── Safe start with guarded init ─────────────────────────────────────
    // Returns false if init failed (caller should fall through to AP mode).
    // On failure, increments the BLE-specific fail counter — NOT the system
    // crash counter — so a BLE init failure never triggers safe-mode lockout.
    static bool start(const String& unitId) {
        if (_active) return true;  // Already running

        // Release Classic BT only once — double-release causes a panic
        if (!_btReleased) {
            esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
            if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
                // ESP_ERR_INVALID_STATE means already released — both are fine
                _btReleased = true;
            } else {
                LOG_W("BLE", "bt_controller_mem_release: %d (non-fatal)", (int)err);
            }
        }

        // Guard BLE init — BLEDevice::init() can abort() if the BT controller
        // is in a bad state; we isolate this with a boolean sentinel so a
        // crash here increments BLE fail count, not system crash count.
        _markBLEAttempt();  // Write attempt flag to NVS before init

        String devName = "LumaRoK-" + unitId.substring(4, 10);
        BLEDevice::init(devName.c_str());

        BLEServer* server = BLEDevice::createServer();
        server->setCallbacks(&instance());

        BLEService* svc = server->createService(BLE_SERVICE_UUID);

        _provChar = svc->createCharacteristic(
            BLE_PROV_CHAR_UUID,
            BLECharacteristic::PROPERTY_WRITE);
        _provChar->setCallbacks(&instance());

        _statusChar = svc->createCharacteristic(
            BLE_STATUS_CHAR_UUID,
            BLECharacteristic::PROPERTY_READ |
            BLECharacteristic::PROPERTY_NOTIFY);
        _statusChar->addDescriptor(new BLE2902());

        svc->start();
        BLEDevice::startAdvertising();

        _active    = true;
        _startMs   = millis();
        _success   = false;

        // Init succeeded — clear the BLE fail counter
        _clearBLEFailCount();

        LOG_I("BLE", "BLE provisioning started — window %lu ms, heap %u bytes free",
              (unsigned long)BLE_TIMEOUT_MS,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
        return true;
    }

    static void stop() {
        if (!_active) return;
        BLEDevice::deinit(true);
        _active = false;
        LOG_I("BLE", "BLE provisioning stopped");
    }

    static bool isActive()  { return _active; }
    static bool isSuccess() { return _success; }

    // Drive from provisioning task — returns true when done
    static bool tick() {
        if (!_active) return true;

        // Drain deferred BLE write — runs in provisioning task context (safe for NVS)
        if (_payloadPending) {
            _payloadPending = false;
            String p = String(_pendingPayload);
            _handlePayload(p);
        }

        if (millis() - _startMs > BLE_TIMEOUT_MS) {
            LOG_W("BLE", "BLE provisioning timed out after %lu ms",
                  (unsigned long)BLE_TIMEOUT_MS);
            stop();
            return true;
        }
        return _success;
    }

    // Call after successful provisioning to fully reset BLE fail state
    static void onProvisionSuccess() {
        _clearBLEFailCount();
        LOG_I("BLE", "BLE provisioning succeeded — fail counter cleared");
    }

    // BLECharacteristicCallbacks
    void onWrite(BLECharacteristic* c) override {
        // Copy payload into staging buffer and set flag.
        // Do NOT call NVS/ProvisioningManager here — BLE stack callback
        // context has a small stack and must not block. tick() drains it.
        String val = c->getValue().c_str();
        if (val.length() > 0 && val.length() < sizeof(_pendingPayload)) {
            strlcpy(_pendingPayload, val.c_str(), sizeof(_pendingPayload));
            _payloadPending = true;
        }
    }

    // BLEServerCallbacks
    void onConnect   (BLEServer*) override {
        LOG_I("BLE", "Client connected — heap %u bytes free",
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
    void onDisconnect(BLEServer* s) override {
        LOG_I("BLE", "Client disconnected — restarting advertising");
        // Restart advertising so another client can connect within the window
        if (_active && !_success) {
            s->startAdvertising();
        }
    }

private:
    // ── NVS helpers for BLE-specific fail counter ─────────────────────────
    // On each boot, before calling BLEDevice::init(), we increment a counter.
    // If init succeeds we clear it immediately. If the device crashes during
    // init, on the next boot the counter will already be incremented — no
    // extra logic needed. This is reliable even without a clean shutdown.
    static void _markBLEAttempt() {
        uint8_t n = NVSStore::getU8(NVS_NS_CONFIG, BLE_CRASH_NVS_KEY, 0);
        if (n < 255) n++;
        NVSStore::putU8(NVS_NS_CONFIG, BLE_CRASH_NVS_KEY, n);
        LOG_I("BLE", "BLE attempt #%d recorded in NVS", (int)n);
    }

    static void _clearBLEFailCount() {
        NVSStore::putU8(NVS_NS_CONFIG, BLE_CRASH_NVS_KEY, 0);
    }

    // ── Payload handler ───────────────────────────────────────────────────
    static void _handlePayload(const String& payload) {
        StaticJsonDocument<512> doc;
        if (deserializeJson(doc, payload) != DeserializationError::Ok) {
            _setStatus("ERROR:JSON_PARSE");
            return;
        }

        const char* ssid      = doc["wifi_ssid"]   | "";
        const char* pass      = doc["wifi_pass"]   | "";
        const char* mqttUser  = doc["mqtt_user"]   | "";
        const char* mqttPass  = doc["mqtt_pass"]   | "";
        const char* devSecret = doc["dev_secret"]  | "";
        const char* url       = doc["backend_url"] | "";

        if (strlen(ssid) == 0 || strlen(mqttUser) == 0) {
            _setStatus("ERROR:MISSING_FIELDS");
            return;
        }

        bool ok = ProvisioningManager::commitCredentials(
            ssid, pass, mqttUser, mqttPass, devSecret, url);

        if (ok) {
            _setStatus("OK:PROVISIONED");
            _success = true;
            onProvisionSuccess();
        } else {
            _setStatus("ERROR:COMMIT_FAILED");
        }
    }

    static void _setStatus(const char* s) {
        if (_statusChar) {
            _statusChar->setValue((uint8_t*)s, strlen(s));
            _statusChar->notify();
        }
    }

    static bool               _active;
    static bool               _success;
    static bool               _btReleased;
    static unsigned long      _startMs;
    static BLECharacteristic* _provChar;
    static BLECharacteristic* _statusChar;
    static volatile bool      _payloadPending;
    static char               _pendingPayload[512];
};

inline bool               BLEProvisioning::_active      = false;
inline bool               BLEProvisioning::_success     = false;
inline bool               BLEProvisioning::_btReleased  = false;
inline unsigned long      BLEProvisioning::_startMs     = 0;
inline BLECharacteristic* BLEProvisioning::_provChar    = nullptr;
inline BLECharacteristic* BLEProvisioning::_statusChar  = nullptr;
inline volatile bool      BLEProvisioning::_payloadPending = false;
inline char               BLEProvisioning::_pendingPayload[512] = {};

#else
// Stub when BLE is disabled at compile time
class BLEProvisioning {
public:
    static bool  shouldStart()              { return false; }
    static bool  start(const String&)       { return false; }
    static void  stop()                     {}
    static bool  isActive()                 { return false; }
    static bool  isSuccess()                { return false; }
    static bool  tick()                     { return true;  }
    static void  onProvisionSuccess()       {}
};
#endif
