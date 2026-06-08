#pragma once
#include <mbedtls/sha256.h>
#include "../core/Types.h"
#include "../security/HMACVerifier.h"

class OTAVerifier {
public:
    // ── SHA-256 streaming context ─────────────────────────────
    static void sha256Begin() {
        mbedtls_sha256_init(&_ctx);
        mbedtls_sha256_starts(&_ctx, 0);
    }

    static void sha256Update(const uint8_t* data, size_t len) {
        mbedtls_sha256_update(&_ctx, data, len);
    }

    static bool sha256Finish(const char* expectedHex) {
        uint8_t digest[32];
        mbedtls_sha256_finish(&_ctx, digest);
        mbedtls_sha256_free(&_ctx);

        char computed[65] = {};
        for (int i = 0; i < 32; i++) {
            snprintf(computed + i * 2, 3, "%02x", digest[i]);
        }

        bool match = (strncmp(computed, expectedHex, 64) == 0);
        if (!match) {
            LOG_E("OTA", "SHA256 mismatch");
            LOG_E("OTA", "  computed:  %s", computed);
            LOG_E("OTA", "  expected:  %s", expectedHex);
        }
        return match;
    }

    // ── URL host whitelist ────────────────────────────────────
    // FIX v3.2: Replaced suffix-match with EXACT hostname comparison.
    //
    // PREVIOUS VULNERABILITY:
    //   strncmp(hostStart + hostLen - hLen, h, hLen) == 0
    //   — passes for "evil-lumarok-backend.onrender.com" against
    //     "lumarok-backend.onrender.com". Any attacker-controlled
    //     subdomain on whitelisted domains would be accepted.
    //
    // FIX: extract the full hostname and compare it exactly against
    //   the allowlist. No suffix match, no wildcard.
    //   Port stripping added: "host:443/path" correctly resolves to "host".
    static bool isURLAllowed(const char* url) {
        // Only allow HTTPS URLs — fail hard on HTTP
        if (strncmp(url, "https://", 8) != 0) {
            LOG_E("OTA", "URL must use HTTPS: %s", url);
            return false;
        }
        // Exact-match allowlist — no suffix tricks possible
        const char* allowed[] = {
            "lumarok-backend.onrender.com",
            "raw.githubusercontent.com",   // raw file delivery only, not Pages
            "cdn.lumarok.com",
            nullptr
        };
        // Extract host from URL — stop at '/', '?', '#', or end
        const char* hostStart = url + 8;  // skip "https://"
        const char* hostEnd   = hostStart;
        while (*hostEnd && *hostEnd != '/' && *hostEnd != '?' &&
               *hostEnd != '#' && *hostEnd != ':') {
            hostEnd++;
        }
        size_t hostLen = (size_t)(hostEnd - hostStart);
        if (hostLen == 0) {
            LOG_E("OTA", "URL has empty hostname: %s", url);
            return false;
        }
        for (int i = 0; allowed[i] != nullptr; i++) {
            size_t aLen = strlen(allowed[i]);
            // Exact match: lengths must match AND content must match
            if (aLen == hostLen &&
                strncmp(hostStart, allowed[i], hostLen) == 0) {
                return true;
            }
        }
        LOG_E("OTA", "URL host not whitelisted: %.*s", (int)hostLen, hostStart);
        return false;
    }

private:
    static mbedtls_sha256_context _ctx;
};

inline mbedtls_sha256_context OTAVerifier::_ctx;
