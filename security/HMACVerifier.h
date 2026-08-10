#pragma once
#include <Arduino.h>
#include <mbedtls/md.h>
#include "../core/Types.h"

class HMACVerifier {
public:
    // Verify HMAC-SHA256 signature.
    // message  = canonical string that was signed
    // key      = dev_secret
    // sigHex   = 64-char hex string received in MQTT command
    static bool verify(const String& message, const String& key, const String& sigHex) {
        if (sigHex.length() != 64) {
            LOG_W("HMAC", "Signature length invalid (%d)", (int)sigHex.length());
            return false;
        }

        uint8_t computed[32];
        if (!_computeHMAC(message, key, computed)) return false;

        // Convert sigHex to bytes
        uint8_t expected[32];
        for (int i = 0; i < 32; i++) {
            char b[3] = { sigHex[i*2], sigHex[i*2+1], 0 };
            expected[i] = (uint8_t)strtol(b, nullptr, 16);
        }

        // Constant-time comparison
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (computed[i] ^ expected[i]);
        if (diff != 0) {
            LOG_W("HMAC", "Signature mismatch");
            return false;
        }
        return true;
    }

    // Build the canonical message string for OTA commands
    static String buildOTAMessage(const String& url, const String& version,
                                   const String& sha256, long ts) {
        return url + ":" + version + ":" + sha256 + ":" + String(ts);
    }

    // Build canonical message for credential rotation
    static String buildCredRotateMessage(const String& mqttUser,
                                          const String& mqttPass,
                                          const String& devSecret, long ts) {
        return mqttUser + ":" + mqttPass + ":" + devSecret + ":" + String(ts);
    }

    // Build the canonical message string for per-device commands
    // (relay/servo/stepper on/off/toggle/set). Format: "action:room:device:ts"
    static String buildDeviceCommandMessage(const String& action, const String& room,
                                             const String& device, long ts) {
        return action + ":" + room + ":" + device + ":" + String(ts);
    }

private:
    static bool _computeHMAC(const String& msg, const String& key, uint8_t out[32]) {
        const mbedtls_md_info_t* info =
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        mbedtls_md_context_t ctx;
        mbedtls_md_init(&ctx);

        if (mbedtls_md_setup(&ctx, info, 1) != 0) {
            mbedtls_md_free(&ctx);
            LOG_E("HMAC", "mbedtls_md_setup failed");
            return false;
        }

        mbedtls_md_hmac_starts(&ctx,
            (const unsigned char*)key.c_str(), key.length());
        mbedtls_md_hmac_update(&ctx,
            (const unsigned char*)msg.c_str(), msg.length());
        mbedtls_md_hmac_finish(&ctx, out);
        mbedtls_md_free(&ctx);
        return true;
    }
};
