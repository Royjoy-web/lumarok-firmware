#pragma once
#include <Preferences.h>
#include <Arduino.h>

// NVSStore — all NVS read/write operations pass through here.
// No other file may call prefs.begin()/end() directly.
// Single open/close cycle per operation; namespace is a parameter.
class NVSStore {
public:
    // ── String ────────────────────────────────────────────────
    static String getString(const char* ns, const char* key, const char* def = "") {
        Preferences p; p.begin(ns, true);
        String v = p.getString(key, def);
        p.end(); return v;
    }

    static bool putString(const char* ns, const char* key, const String& val) {
        Preferences p; p.begin(ns, false);
        bool ok = p.putString(key, val) > 0;
        p.end(); return ok;
    }

    static bool putString(const char* ns, const char* key, const char* val) {
        return putString(ns, key, String(val));
    }

    // ── Bool ──────────────────────────────────────────────────
    static bool getBool(const char* ns, const char* key, bool def = false) {
        Preferences p; p.begin(ns, true);
        bool v = p.getBool(key, def);
        p.end(); return v;
    }

    static bool putBool(const char* ns, const char* key, bool val) {
        Preferences p; p.begin(ns, false);
        bool ok = p.putBool(key, val);
        p.end(); return ok;
    }

    // ── UInt8 ─────────────────────────────────────────────────
    static uint8_t getU8(const char* ns, const char* key, uint8_t def = 0) {
        Preferences p; p.begin(ns, true);
        uint8_t v = p.getUChar(key, def);
        p.end(); return v;
    }

    static bool putU8(const char* ns, const char* key, uint8_t val) {
        Preferences p; p.begin(ns, false);
        bool ok = p.putUChar(key, val);
        p.end(); return ok;
    }

    // ── UInt32 ────────────────────────────────────────────────
    static uint32_t getU32(const char* ns, const char* key, uint32_t def = 0) {
        Preferences p; p.begin(ns, true);
        uint32_t v = p.getUInt(key, def);
        p.end(); return v;
    }

    static bool putU32(const char* ns, const char* key, uint32_t val) {
        Preferences p; p.begin(ns, false);
        bool ok = p.putUInt(key, val);
        p.end(); return ok;
    }

    // ── Blob ──────────────────────────────────────────────────
    static size_t getBlob(const char* ns, const char* key, void* buf, size_t len) {
        Preferences p; p.begin(ns, true);
        size_t n = p.getBytes(key, buf, len);
        p.end(); return n;
    }

    static bool putBlob(const char* ns, const char* key, const void* buf, size_t len) {
        Preferences p; p.begin(ns, false);
        bool ok = p.putBytes(key, buf, len) == len;
        p.end(); return ok;
    }

    // ── Existence check ───────────────────────────────────────
    static bool exists(const char* ns, const char* key) {
        Preferences p; p.begin(ns, true);
        bool found = p.isKey(key);
        p.end(); return found;
    }

    // ── Remove ────────────────────────────────────────────────
    static bool remove(const char* ns, const char* key) {
        Preferences p; p.begin(ns, false);
        bool ok = p.remove(key);
        p.end(); return ok;
    }

    // ── Clear namespace ───────────────────────────────────────
    static bool clearNamespace(const char* ns) {
        Preferences p; p.begin(ns, false);
        bool ok = p.clear();
        p.end(); return ok;
    }
};
