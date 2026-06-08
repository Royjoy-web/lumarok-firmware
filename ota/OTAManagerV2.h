#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/TaskManager.h"
#include "../core/RetryPolicy.h"
#include "../core/Identity.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../ota/OTAVerifier.h"
#include "../ota/RollbackManager.h"
#include "../diagnostics/FaultManager.h"
#include "../storage/FlashWearGuard.h"
#include "../storage/NVSStore.h"

#define OTA_MAX_RETRIES   3
#define OTA_CHUNK_SIZE    4096
#define OTA_NVS_CHECKPOINT "ota_chk"     // resume checkpoint (future)
#define OTA_NVS_ATTEMPTS   "ota_att"

enum class OTAState : uint8_t {
    IDLE, PREPARING, DOWNLOADING, VERIFYING, APPLYING, FAILED, ROLLBACK_CHECK
};

class OTAManagerV2 {
public:
    static void init() {
        _state    = OTAState::IDLE;
        _attempts = NVSStore::getU8(NVS_NS_CONFIG, OTA_NVS_ATTEMPTS, 0);
        if (_attempts > 0)
            LOG_W("OTA", "Previous OTA attempts: %d", (int)_attempts);
        // Auto-rollback if too many failed attempts
        if (_attempts >= OTA_MAX_RETRIES) {
            LOG_W("OTA", "Max OTA attempts reached — triggering rollback check");
            _state = OTAState::ROLLBACK_CHECK;
        }
    }

    static void begin(const OTACommand& cmd) {
        if (_state != OTAState::IDLE) return;
        _cmd       = cmd;
        _attempts++;
        NVSStore::putU8(NVS_NS_CONFIG, OTA_NVS_ATTEMPTS, _attempts);
        _state     = OTAState::PREPARING;
        LOG_I("OTA2", "OTA begin: %s  v%s  (attempt %d/%d)",
              cmd.url, cmd.version, (int)_attempts, OTA_MAX_RETRIES);
    }

    // Returns true when terminal (success reboots; failure returns true)
    static bool tick() {
        switch (_state) {
            case OTAState::IDLE:           return false;
            case OTAState::ROLLBACK_CHECK: _doRollbackCheck(); return true;
            case OTAState::PREPARING:      _doPrepare();       break;
            case OTAState::DOWNLOADING:    _doDownload();      break;
            case OTAState::VERIFYING:      _doVerify();        break;
            case OTAState::APPLYING:       _doApply();         break;
            case OTAState::FAILED:         _doFailed();        return true;
        }
        return false;
    }

    static bool     isActive()   { return _state != OTAState::IDLE; }
    static OTAState state()      { return _state; }

private:
    static void _doPrepare() {
        if (!OTAVerifier::isURLAllowed(_cmd.url)) {
            _fail("URL rejected"); return;
        }
        // Check free space: need at least 1.9 MB in OTA partition
        const esp_partition_t* update_part =
            esp_ota_get_next_update_partition(NULL);
        if (!update_part) { _fail("No OTA partition"); return; }

        _otaTLS.setCACert(MQTT_ROOT_CA);
        _otaTLS.setTimeout(15);
        OTAVerifier::sha256Begin();
        _bytesWritten = 0;
        EventBus::setNetBit(NET_OTA_ACTIVE_BIT);
        _publishProgress(0, "preparing");
        _state = OTAState::DOWNLOADING;
    }

    static void _doDownload() {
        HTTPClient http;
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setTimeout(20000);
        http.setReuse(false);
        http.addHeader("User-Agent",      "LumaRoK/" FIRMWARE_VERSION);
        http.addHeader("X-Unit-Id",       Identity::get().c_str());
        http.addHeader("X-FW-Version",    FIRMWARE_VERSION);

        if (!http.begin(_otaTLS, _cmd.url)) {
            _fail("HTTPClient.begin"); return;
        }

        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            http.end();
            char m[24]; snprintf(m, sizeof(m), "HTTP %d", code);
            _fail(m); return;
        }

        int total = http.getSize();
        if (!Update.begin(total > 0 ? total : UPDATE_SIZE_UNKNOWN)) {
            http.end();
            _fail("Update.begin"); return;
        }

        WiFiClient* stream = http.getStreamPtr();
        static uint8_t chunk[OTA_CHUNK_SIZE];
        int lastPct = -1;

        while (http.connected()) {
            // Feed hardware watchdog on every chunk — critical fix
            TaskManager::feedWatchdog();

            size_t avail = stream->available();
            if (!avail) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }

            size_t n = stream->readBytes(chunk, min(avail, sizeof(chunk)));
            if (n == 0) break;

            OTAVerifier::sha256Update(chunk, n);
            if (Update.write(chunk, n) != n) {
                http.end(); _fail("write mismatch"); return;
            }
            _bytesWritten += n;

            if (total > 0) {
                int pct = (int)((_bytesWritten * 100) / (size_t)total);
                if (pct / 5 != lastPct / 5) {   // every 5%
                    lastPct = pct;
                    _publishProgress(pct, "downloading");
                }
            }

            // Early termination: size known and complete
            if (total > 0 && _bytesWritten >= (size_t)total) break;
        }
        http.end();

        if (total > 0 && _bytesWritten != (size_t)total) {
            _fail("incomplete"); return;
        }
        LOG_I("OTA2", "Download: %u bytes", (unsigned)_bytesWritten);
        _state = OTAState::VERIFYING;
    }

    static void _doVerify() {
        _publishProgress(100, "verifying");

        // ── SHA-256 check ─────────────────────────────────────
        if (!OTAVerifier::sha256Finish(_cmd.sha256)) {
            Update.abort();
            _fail("sha256 mismatch"); return;
        }

        // ── Partition sanity: validate the written image header ─
        if (!Update.isFinished()) {
            Update.abort();
            _fail("update not finished"); return;
        }

        LOG_I("OTA2", "Verification passed");
        _state = OTAState::APPLYING;
    }

    static void _doApply() {
        // Flush all pending NVS writes before OTA-triggered restart
        FlashWearGuard::flushAll();

        if (!Update.end(true)) {
            _fail("Update.end"); return;
        }

        // Clear attempt counter — new image starts clean
        NVSStore::putU8(NVS_NS_CONFIG, OTA_NVS_ATTEMPTS, 0);
        RollbackManager::markOTAComplete(_cmd.version);
        _publishProgress(100, "complete");

        LOG_I("OTA2", "Applied v%s — restarting in 2s", _cmd.version);
        vTaskDelay(pdMS_TO_TICKS(2000));
        MQTTTransport::disconnect();
        EventBus::clearNetBit(NET_OTA_ACTIVE_BIT);
        esp_restart();
    }

    static void _doRollbackCheck() {
        LOG_W("OTA2", "Max attempts exhausted — rolling back");
        NVSStore::putU8(NVS_NS_CONFIG, OTA_NVS_ATTEMPTS, 0);
        _publishProgress(0, "rollback");
        if (Update.canRollBack()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            Update.rollBack();
            esp_restart();
        } else {
            LOG_E("OTA2", "Rollback not available — continuing with current image");
            _state = OTAState::IDLE;
        }
    }

    static void _fail(const char* reason) {
        LOG_E("OTA2", "OTA FAILED: %s (attempt %d)", reason, (int)_attempts);
        Update.abort();
        _otaTLS.stop();
        FaultManager::record(FaultCode::OTA_FAIL, reason, FaultSeverity::SEV_HIGH);
        _publishProgress(0, reason);
        EventBus::clearNetBit(NET_OTA_ACTIVE_BIT);
        _state = OTAState::FAILED;
    }

    static void _doFailed() {
        _otaTLS.stop();
        _state = OTAState::IDLE;
    }

    static void _publishProgress(int pct, const char* status) {
        char buf[160];
        snprintf(buf, sizeof(buf),
            "{\"pct\":%d,\"status\":\"%s\",\"ver\":\"%s\","
            "\"attempt\":%d,\"bytes\":%u,\"unit\":\"%s\"}",
            pct, status, _cmd.version, (int)_attempts,
            (unsigned)_bytesWritten, Identity::get().c_str());
        MQTTTransport::publish(MQTTTopics::otaProgress(), buf, 0, false);
    }

    static OTAState         _state;
    static OTACommand       _cmd;
    static WiFiClientSecure _otaTLS;
    static size_t           _bytesWritten;
    static uint8_t          _attempts;
};

inline OTAState         OTAManagerV2::_state        = OTAState::IDLE;
inline OTACommand       OTAManagerV2::_cmd          = {};
inline WiFiClientSecure OTAManagerV2::_otaTLS;
inline size_t           OTAManagerV2::_bytesWritten = 0;
inline uint8_t          OTAManagerV2::_attempts     = 0;
