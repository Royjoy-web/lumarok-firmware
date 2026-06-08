#pragma once
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/TaskManager.h"
#include "../core/Identity.h"
#include "../mqtt/MQTTTransport.h"
#include "../mqtt/MQTTTopics.h"
#include "../ota/OTAVerifier.h"
#include "../ota/RollbackManager.h"

enum class OTAState : uint8_t { IDLE, PREPARING, DOWNLOADING, VERIFYING, APPLYING, FAILED };

class OTAManager {
public:
    static void init() {
        LOG_I("OTA", "OTA manager ready (current fw: %s)", FIRMWARE_VERSION);
    }

    // Entry point — called when OTA_COMMAND event arrives in OTATask
    static void begin(const OTACommand& cmd) {
        if (_state != OTAState::IDLE) {
            LOG_W("OTA", "OTA already in progress — ignoring");
            return;
        }
        _cmd   = cmd;
        _state = OTAState::PREPARING;
        LOG_I("OTA", "Starting OTA → %s  version: %s", cmd.url, cmd.version);
    }

    // Drive the state machine — called repeatedly from OTATask loop
    // Returns true when complete (success or failure)
    static bool tick() {
        switch (_state) {
            case OTAState::IDLE:        return false;
            case OTAState::PREPARING:   _doPrepare();   break;
            case OTAState::DOWNLOADING: _doDownload();  break;
            case OTAState::VERIFYING:   _doVerify();    break;
            case OTAState::APPLYING:    _doApply();     break;
            case OTAState::FAILED:      _doCleanup();   return true;
            default: break;
        }
        return false;
    }

    static OTAState state()    { return _state; }
    static bool     isActive() { return _state != OTAState::IDLE; }

private:
    // ── PREPARING ─────────────────────────────────────────────
    static void _doPrepare() {
        if (!OTAVerifier::isURLAllowed(_cmd.url)) {
            _fail("URL not allowed");
            return;
        }
        EventBus::setNetBit(NET_OTA_ACTIVE_BIT);
        _publishProgress(0, "preparing");

        // OTA uses its own TLS client — NEVER shares with MQTTTransport
        _otaTLS.setCACert(MQTT_ROOT_CA);

        OTAVerifier::sha256Begin();
        _bytesWritten = 0;
        _state        = OTAState::DOWNLOADING;
        LOG_I("OTA", "Prepared — beginning download");
    }

    // ── DOWNLOADING ───────────────────────────────────────────
    static void _doDownload() {
        HTTPClient http;
        if (!http.begin(_otaTLS, _cmd.url)) {
            _fail("HTTPClient.begin failed");
            return;
        }
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
        http.setTimeout(30000);
        http.addHeader("User-Agent", "LumaRoK/" FIRMWARE_VERSION);

        int httpCode = http.GET();
        if (httpCode != HTTP_CODE_OK) {
            http.end();
            char msg[48]; snprintf(msg, sizeof(msg), "HTTP %d", httpCode);
            _fail(msg);
            return;
        }

        int totalSize = http.getSize();
        if (totalSize > 0 && !Update.begin(totalSize)) {
            http.end();
            _fail("Update.begin failed (partition full?)");
            return;
        } else if (totalSize <= 0) {
            Update.begin(UPDATE_SIZE_UNKNOWN);
        }

        WiFiClient* stream = http.getStreamPtr();
        uint8_t     chunk[4096];
        int         lastPct  = -1;

        while (http.connected() && (_bytesWritten < (size_t)totalSize || totalSize <= 0)) {
            // Feed watchdog on every chunk — this is the critical fix
            TaskManager::feedWatchdog();

            size_t avail = stream->available();
            if (!avail) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }

            size_t toRead = min(avail, sizeof(chunk));
            size_t n      = stream->readBytes(chunk, toRead);
            if (n == 0) break;

            OTAVerifier::sha256Update(chunk, n);
            size_t written = Update.write(chunk, n);
            if (written != n) {
                http.end();
                _fail("Update.write mismatch");
                return;
            }

            _bytesWritten += n;

            // Progress every 10%
            if (totalSize > 0) {
                int pct = (int)((_bytesWritten * 100) / (size_t)totalSize);
                if (pct / 10 != lastPct / 10) {
                    lastPct = pct;
                    _publishProgress(pct, "downloading");
                }
            }
        }

        http.end();

        if (totalSize > 0 && _bytesWritten != (size_t)totalSize) {
            _fail("Incomplete download");
            return;
        }

        LOG_I("OTA", "Download complete: %u bytes", (unsigned)_bytesWritten);
        _state = OTAState::VERIFYING;
    }

    // ── VERIFYING ─────────────────────────────────────────────
    static void _doVerify() {
        _publishProgress(100, "verifying");
        if (!OTAVerifier::sha256Finish(_cmd.sha256)) {
            Update.abort();
            _fail("SHA256 mismatch — firmware rejected");
            return;
        }
        LOG_I("OTA", "SHA256 verified OK");
        _state = OTAState::APPLYING;
    }

    // ── APPLYING ──────────────────────────────────────────────
    static void _doApply() {
        if (!Update.end(true)) {
            _fail("Update.end failed");
            return;
        }
        RollbackManager::markOTAComplete(_cmd.version);
        _publishProgress(100, "complete");

        LOG_I("OTA", "Applying — restarting in 2s");
        vTaskDelay(pdMS_TO_TICKS(2000));   // Allow MQTT publish to flush
        MQTTTransport::disconnect();
        EventBus::clearNetBit(NET_OTA_ACTIVE_BIT);
        esp_restart();
    }

    // ── FAILURE ───────────────────────────────────────────────
    static void _fail(const char* reason) {
        LOG_E("OTA", "OTA failed: %s", reason);
        Update.abort();
        _publishProgress(0, reason);
        _state = OTAState::FAILED;
    }

    static void _doCleanup() {
        _otaTLS.stop();
        EventBus::clearNetBit(NET_OTA_ACTIVE_BIT);
        _state = OTAState::IDLE;
    }

    static void _publishProgress(int pct, const char* status) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"percent\":%d,\"status\":\"%s\",\"version\":\"%s\",\"unit_id\":\"%s\"}",
                 pct, status, _cmd.version, Identity::get().c_str());
        MQTTTransport::publish(MQTTTopics::otaProgress(), buf, 0, false);
        LOG_I("OTA", "[%3d%%] %s", pct, status);
    }

    static OTAState        _state;
    static OTACommand      _cmd;
    static WiFiClientSecure _otaTLS;   // Dedicated TLS client — never shared
    static size_t          _bytesWritten;
};

inline OTAState         OTAManager::_state        = OTAState::IDLE;
inline OTACommand       OTAManager::_cmd          = {};
inline WiFiClientSecure OTAManager::_otaTLS;
inline size_t           OTAManager::_bytesWritten = 0;
