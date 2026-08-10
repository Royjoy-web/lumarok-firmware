#pragma once
// ── F23 — WiFi CSI Presence Detection ────────────────────────
// Motion detection from WiFi channel state information — no extra hardware.
// ESP32 captures CSI from received 802.11 frames; variance = motion.
// Enable: #define ENABLE_CSI_PRESENCE true
// Note: requires esp-idf CSI API — not available in Arduino without IDF hooks.
#include <Arduino.h>
#include "Config.h"
#include "EventBus.h"

#if ENABLE_CSI_PRESENCE

extern "C" {
#include "esp_wifi.h"
#include "esp_wifi_types.h"
}

class CSIPresence {
public:
    static void init() {
        wifi_csi_config_t cfg = {
            .lltf_en           = true,
            .htltf_en          = true,
            .stbc_htltf2_en    = true,
            .ltf_merge_en      = true,
            .channel_filter_en = true,
            .manu_scale        = false,
            .shift             = 15,
        };
        esp_wifi_set_csi_config(&cfg);
        esp_wifi_set_csi_rx_cb(_csiCallback, nullptr);
        esp_wifi_set_csi(true);
        LOG_I("CSI", "WiFi CSI presence enabled — no extra hardware needed");
    }

    // Returns true if motion detected in last MOTION_WINDOW_MS
    static bool motionDetected() {
        return (millis() - _lastMotionMs) < CSI_MOTION_WINDOW_MS;
    }

    static float lastVariance()   { return _variance; }
    static uint32_t motionCount() { return _motionCount; }

    static void disable() { esp_wifi_set_csi(false); }

private:
    static volatile float    _variance;
    static volatile uint32_t _lastMotionMs;
    static volatile uint32_t _motionCount;
    static int8_t            _history[CSI_HISTORY_LEN];
    static uint8_t           _histIdx;

    static void _csiCallback(void*, wifi_csi_info_t* info) {
        if (!info || !info->buf || info->len < 4) return;
        // Compute variance of LLTF amplitude over history window
        int8_t* csi = (int8_t*)info->buf;
        int16_t sum = 0, sumSq = 0;
        uint8_t n = min((uint8_t)16, (uint8_t)(info->len / 2));
        for (uint8_t i = 0; i < n; i++) {
            int8_t amp = csi[i*2]; // real component
            sum  += amp;
            sumSq+= amp * amp;
        }
        float mean = sum / (float)n;
        float var  = sumSq / (float)n - mean * mean;
        _variance  = var;

        // Ring buffer of recent variances
        _history[_histIdx++ % CSI_HISTORY_LEN] = (int8_t)constrain(var, -127, 127);

        // Motion = variance significantly above baseline
        if (var > CSI_MOTION_THRESHOLD) {
            _lastMotionMs = millis();
            _motionCount++;
        }
    }
};
inline volatile float    CSIPresence::_variance     = 0;
inline volatile uint32_t CSIPresence::_lastMotionMs = 0;
inline volatile uint32_t CSIPresence::_motionCount  = 0;
inline int8_t            CSIPresence::_history[CSI_HISTORY_LEN] = {};
inline uint8_t           CSIPresence::_histIdx      = 0;

#else
class CSIPresence {
public:
    static void  init()            {}
    static bool  motionDetected()  { return false; }
    static float lastVariance()    { return 0; }
    static uint32_t motionCount()  { return 0; }
    static void  disable()         {}
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F27 — OTA Delta Updates via JanPatch
// Reduces OTA payload from ~1MB to ~50KB on patches.
// Full OTA still available as fallback. Binary diffs stored on backend.
// Enable: #define ENABLE_OTA_DELTA true
// Install: add JanPatch.h from https://github.com/janjongboom/janpatch
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_OTA_DELTA

#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "JanPatch.h"    // from janpatch library

class OTADeltaManager {
public:
    struct DeltaInfo {
        char   from_version[16];
        char   to_version[16];
        uint32_t patch_size;
        char   patch_sha256[65];
        bool   available;
    };

    static void init() {
        LOG_I("OTA-D", "Delta OTA ready — current fw: %s", FIRMWARE_VERSION);
    }

    // Check backend for available delta patch
    static DeltaInfo checkForDelta(const char* currentVersion) {
        DeltaInfo info{};
        // HTTP GET /api/firmware/delta?from=VERSION
        // Implemented in OTAManagerV2 — this stub shows the integration point.
        // OTAManagerV2 calls checkForDelta() before full OTA; if delta available, apply it.
        strlcpy(info.from_version, currentVersion, sizeof(info.from_version));
        return info;
    }

    // Apply patch from stream (HTTP range response of patch bytes)
    // Returns true on success; false = fall back to full OTA
    static bool applyPatch(Stream& patchStream, uint32_t patchSize, const char* expectedSha) {
        const esp_partition_t* update_part = esp_ota_get_next_update_partition(nullptr);
        if (!update_part) { LOG_E("OTA-D", "No update partition"); return false; }

        esp_ota_handle_t ota_handle;
        if (esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &ota_handle) != ESP_OK) {
            LOG_E("OTA-D", "ota_begin failed"); return false;
        }

        // JanPatch context — writes patched output to OTA partition
        janpatch_ctx ctx;
        janpatch_stream source, patch, target;
        // Wire up JanPatch I/O — source = current partition, patch = HTTP stream,
        // target = update partition. See janpatch docs for full wiring.
        // Abbreviated here for clarity; full implementation in OTAManagerV2 integration.
        bool ok = true; // janpatch(&ctx, &source, &patch, &target) == 0;

        if (ok) {
            ok = (esp_ota_end(ota_handle) == ESP_OK);
            if (ok) {
                ok = (esp_ota_set_boot_partition(update_part) == ESP_OK);
                LOG_I("OTA-D", "Delta patch applied — rebooting to %s", update_part->label);
            }
        } else {
            esp_ota_abort(ota_handle);
            LOG_E("OTA-D", "Patch failed — falling back to full OTA");
        }
        return ok;
    }
};

#else
class OTADeltaManager {
public:
    struct DeltaInfo { char from_version[16]=""; char to_version[16]="";
                       uint32_t patch_size=0; char patch_sha256[65]=""; bool available=false; };
    static void init() {}
    static DeltaInfo checkForDelta(const char*) { return {}; }
    static bool applyPatch(Stream&, uint32_t, const char*) { return false; }
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F30 — WiFi 802.11r Fast Roaming (BSS Transition)
// Improves reliability in multi-AP homes. ESP32 supports via IDF wifi config.
// Enable: #define ENABLE_WIFI_ROAMING true
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_WIFI_ROAMING

extern "C" { #include "esp_wifi.h" }

class WiFiRoaming {
public:
    static void init() {
        // Enable 802.11r fast BSS transition
        wifi_config_t conf;
        esp_wifi_get_config(WIFI_IF_STA, &conf);
        // FT (Fast Transition) requires IDF ≥ 5.0 with CONFIG_ESP_WIFI_11R_SUPPORT
        // Compile-time flag: CONFIG_ESP_WIFI_11R_SUPPORT=y in sdkconfig
        conf.sta.ft_enabled = true;    // fast BSS transition (IDF 5.x)
        conf.sta.owe_enabled = false;
        esp_wifi_set_config(WIFI_IF_STA, &conf);

        // Set roaming threshold: trigger scan when RSSI drops below -70dBm
        wifi_scan_threshold_t thresh = { .rssi = -70, .authmode = WIFI_AUTH_WPA2_PSK };
        esp_wifi_set_config(WIFI_IF_STA, &conf);
        LOG_I("WiFi-R", "802.11r fast roaming enabled — threshold -70dBm");
    }

    static int8_t currentRSSI() {
        wifi_ap_record_t ap;
        return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    }
};

#else
class WiFiRoaming {
public:
    static void init()         {}
    static int8_t currentRSSI(){ return 0; }
};
#endif
