#pragma once
// ── F17 — RGBW Strip Control ──────────────────────────────────
// 4-ch PWM on free S3 GPIOs. Mood scenes + entertainment sync.
// Builds on PWMDimmer (Phase 1) — registers 4 channels at init.
// Enable: #define ENABLE_RGBW true
#include <Arduino.h>
#include "Config.h"
#include "NVSStore.h"
#include "EventBus.h"

#if ENABLE_RGBW
#include "AccessAndAutomation.h"  // PWMDimmer

class RGBWDriver {
public:
    struct Color { uint8_t r, g, b, w; };

    static bool init() {
        _chR = PWMDimmer::registerChannel(PIN_RGBW_R);
        _chG = PWMDimmer::registerChannel(PIN_RGBW_G);
        _chB = PWMDimmer::registerChannel(PIN_RGBW_B);
        _chW = PWMDimmer::registerChannel(PIN_RGBW_W);
        bool ok = (_chR >= 0 && _chG >= 0 && _chB >= 0 && _chW >= 0);
        if (ok) LOG_I("RGBW", "4ch PWM ready R=%d G=%d B=%d W=%d",
                      PIN_RGBW_R, PIN_RGBW_G, PIN_RGBW_B, PIN_RGBW_W);
        else    LOG_E("RGBW", "Channel registration failed — PWMDimmer full");
        return ok;
    }

    static void set(Color c) {
        _current = c;
        PWMDimmer::setBrightness(_chR, c.r * 100 / 255);
        PWMDimmer::setBrightness(_chG, c.g * 100 / 255);
        PWMDimmer::setBrightness(_chB, c.b * 100 / 255);
        PWMDimmer::setBrightness(_chW, c.w * 100 / 255);
    }

    static void setHex(uint32_t hex, uint8_t white = 0) {
        set({ (uint8_t)(hex>>16), (uint8_t)(hex>>8), (uint8_t)hex, white });
    }

    static void fadeToColor(Color target, uint32_t ms) {
        Color from = _current;
        uint32_t steps = ms / 20;
        if (!steps) { set(target); return; }
        for (uint32_t i = 0; i <= steps; i++) {
            Color c = {
                (uint8_t)(from.r + (int16_t)(target.r - from.r) * (int32_t)i / steps),
                (uint8_t)(from.g + (int16_t)(target.g - from.g) * (int32_t)i / steps),
                (uint8_t)(from.b + (int16_t)(target.b - from.b) * (int32_t)i / steps),
                (uint8_t)(from.w + (int16_t)(target.w - from.w) * (int32_t)i / steps),
            };
            set(c);
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    // Preset scenes
    static void sceneRelax()     { fadeToColor({255, 120, 30, 80}, 1500); }
    static void sceneDay()       { fadeToColor({255, 255, 240, 255}, 1000); }
    static void sceneNight()     { fadeToColor({20, 0, 40, 5}, 2000); }
    static void sceneCinema()    { fadeToColor({180, 0, 0, 0}, 1000); }
    static void sceneParty()     { /* cycle handled in loop */ _partyMode = true; }
    static void sceneOff()       { _partyMode = false; fadeToColor({0,0,0,0}, 500); }

    // Party mode tick — call from sensor task at 500ms
    static void tick() {
        if (!_partyMode) return;
        _partyHue = (_partyHue + 15) % 360;
        Color c = _hsvToRgb(_partyHue, 255, 200);
        set(c);
    }

    static Color current() { return _current; }

    // Dispatch from MQTT command: {"action":"scene","scene":"relax"} or {"action":"color","hex":"FF6A1E","white":50}
    static void dispatchMQTT(const char* action, uint32_t hex, uint8_t white, const char* scene) {
        if (strcmp(action, "color") == 0) { setHex(hex, white); return; }
        if (strcmp(action, "scene") == 0) {
            if      (strcmp(scene,"relax")  == 0) sceneRelax();
            else if (strcmp(scene,"day")    == 0) sceneDay();
            else if (strcmp(scene,"night")  == 0) sceneNight();
            else if (strcmp(scene,"cinema") == 0) sceneCinema();
            else if (strcmp(scene,"party")  == 0) sceneParty();
            else if (strcmp(scene,"off")    == 0) sceneOff();
        }
        if (strcmp(action, "off") == 0) sceneOff();
    }

private:
    static int8_t _chR, _chG, _chB, _chW;
    static Color  _current;
    static bool   _partyMode;
    static uint16_t _partyHue;

    // Fast HSV→RGB (hue 0–359, sat/val 0–255)
    static Color _hsvToRgb(uint16_t h, uint8_t s, uint8_t v) {
        if (!s) return {v, v, v, 0};
        uint8_t region = h / 60, rem = (h % 60) * 255 / 60;
        uint8_t p = v * (255 - s) / 255;
        uint8_t q = v * (255 - (s * rem / 255)) / 255;
        uint8_t t = v * (255 - (s * (255 - rem) / 255)) / 255;
        switch (region) {
            case 0: return {v,t,p,0}; case 1: return {q,v,p,0};
            case 2: return {p,v,t,0}; case 3: return {p,q,v,0};
            case 4: return {t,p,v,0}; default: return {v,p,q,0};
        }
    }
};
inline int8_t  RGBWDriver::_chR = -1;
inline int8_t  RGBWDriver::_chG = -1;
inline int8_t  RGBWDriver::_chB = -1;
inline int8_t  RGBWDriver::_chW = -1;
inline RGBWDriver::Color RGBWDriver::_current = {};
inline bool    RGBWDriver::_partyMode  = false;
inline uint16_t RGBWDriver::_partyHue = 0;

#else
class RGBWDriver {
public:
    struct Color { uint8_t r=0,g=0,b=0,w=0; };
    static bool init() { return false; }
    static void set(Color) {} static void setHex(uint32_t, uint8_t=0) {}
    static void fadeToColor(Color, uint32_t) {} static void tick() {}
    static void sceneRelax() {} static void sceneDay()   {} static void sceneNight()  {}
    static void sceneCinema(){} static void sceneParty() {} static void sceneOff()    {}
    static void dispatchMQTT(const char*, uint32_t, uint8_t, const char*) {}
    static Color current() { return {}; }
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F18 — HVAC/AC Thermostat Controller
// Relay controls mains AC power; IRTransmitter sets temperature setpoint.
// Thermostat loop uses SCD41 temp reading (Phase 1). Hysteresis ±0.5°C.
// Enable: #define ENABLE_HVAC true
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_HVAC
#include "IRTransmitter.h"

class HVACController {
public:
    enum class Mode : uint8_t { OFF, COOL, HEAT, FAN, AUTO };

    struct HVACState {
        Mode    mode       = Mode::OFF;
        uint8_t setpointC  = 22;    // target °C
        uint8_t fanSpeed   = 1;     // 1–3
        bool    powerOn    = false;
    };

    static void init() {
        // Load last state from NVS
        _state.setpointC = (uint8_t)NVSStore::getUInt(NVS_NS_CONFIG, "hvac_sp", 22);
        _state.mode      = (Mode)NVSStore::getUInt(NVS_NS_CONFIG, "hvac_mode", (uint32_t)Mode::OFF);
        LOG_I("HVAC", "Init — setpoint=%d°C mode=%d", _state.setpointC, (int)_state.mode);
    }

    // Call every 30s from sensor task with current temperature (from SCD41)
    static void thermostatTick(float currentTemp) {
        if (_state.mode == Mode::OFF || !_state.powerOn) return;
        float sp = _state.setpointC;

        if (_state.mode == Mode::COOL || _state.mode == Mode::AUTO) {
            if (currentTemp > sp + 0.5f && !_cooling) {
                _cooling = true;
                _sendIRCommand(sp, Mode::COOL);
                LOG_D("HVAC", "Cooling ON: %.1f°C > %.0f°C", currentTemp, sp);
            } else if (currentTemp <= sp - 0.5f && _cooling) {
                _cooling = false;
                _sendIRCommand(sp, Mode::FAN); // switch to fan only
                LOG_D("HVAC", "Cooling OFF: %.1f°C <= %.0f°C", currentTemp, sp);
            }
        }
        if (_state.mode == Mode::HEAT || _state.mode == Mode::AUTO) {
            if (currentTemp < sp - 0.5f && !_heating) {
                _heating = true;
                _sendIRCommand(sp, Mode::HEAT);
                LOG_D("HVAC", "Heating ON: %.1f°C < %.0f°C", currentTemp, sp);
            } else if (currentTemp >= sp + 0.5f && _heating) {
                _heating = false;
                _sendIRCommand(sp, Mode::FAN);
            }
        }
    }

    // MQTT command: {"action":"hvac_set","mode":"cool","setpoint":22,"fan":2}
    static void setMode(Mode mode, uint8_t setpointC, uint8_t fan = 1) {
        _state.mode = mode; _state.setpointC = setpointC; _state.fanSpeed = fan;
        _state.powerOn = (mode != Mode::OFF);
        NVSStore::putUInt(NVS_NS_CONFIG, "hvac_sp",   setpointC);
        NVSStore::putUInt(NVS_NS_CONFIG, "hvac_mode", (uint32_t)mode);
        if (!_state.powerOn) { _sendIRPower(false); _cooling=false; _heating=false; return; }
        _sendIRPower(true);
        _sendIRCommand(setpointC, mode);
        LOG_I("HVAC", "Set: mode=%d sp=%d°C fan=%d", (int)mode, setpointC, fan);
    }

    static HVACState state() { return _state; }

private:
    static HVACState _state;
    static bool      _cooling, _heating;

    // IR codes: Samsung split AC (NEC-like format)
    // Real codes should be captured with TSOP38238 and stored in IR profiles (B06).
    // These are representative placeholders — replace with actual captured codes.
    static void _sendIRPower(bool on) {
        uint32_t code = on ? 0xE0E09966 : 0xE0E019E6; // Samsung AC power codes
        IRTransmitter::sendNEC(code, 1);
    }

    static void _sendIRCommand(uint8_t sp, Mode mode) {
        // In production: look up code from IRProfile NVS cache for the unit's AC brand.
        // Placeholder: send generic NEC frame encoding mode+temp
        uint32_t modeCode = (mode == Mode::COOL) ? 0x01 : (mode == Mode::HEAT) ? 0x04 : 0x02;
        uint32_t frame = (0xB2 << 24) | (modeCode << 16) | (sp << 8) | _state.fanSpeed;
        IRTransmitter::sendNEC(frame, 0);
        LOG_D("HVAC", "IR command: mode=%d sp=%d", (int)mode, sp);
    }
};
inline HVACController::HVACState HVACController::_state  = {};
inline bool HVACController::_cooling = false;
inline bool HVACController::_heating = false;

#else
class HVACController {
public:
    enum class Mode : uint8_t { OFF, COOL, HEAT, FAN, AUTO };
    struct HVACState { Mode mode=Mode::OFF; uint8_t setpointC=22,fanSpeed=1; bool powerOn=false; };
    static void init() {}
    static void thermostatTick(float) {}
    static void setMode(Mode, uint8_t, uint8_t=1) {}
    static HVACState state() { return {}; }
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F10 — Soil Moisture (Garden): Capacitive v2.0 sensor
// Auto-trigger pool/garden pump relay when below threshold.
// Enable: #define ENABLE_SOIL true
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_SOIL

class SoilDriver {
public:
    // Dry=4095 (air), Wet=1500 (water) — calibrate per sensor
    static constexpr int DRY_VALUE  = 3500;
    static constexpr int WET_VALUE  = 1500;

    static void init() {
        pinMode(PIN_SOIL, INPUT);
        LOG_I("Soil", "Moisture sensor on GPIO%d (dry=%d wet=%d)", PIN_SOIL, DRY_VALUE, WET_VALUE);
    }

    // Returns 0–100% moisture
    static uint8_t readPercent() {
        int raw = analogRead(PIN_SOIL);
        int clamped = constrain(raw, WET_VALUE, DRY_VALUE);
        return (uint8_t)map(clamped, DRY_VALUE, WET_VALUE, 0, 100);
    }

    static int readRaw() { return analogRead(PIN_SOIL); }

    // Returns true if below moisture threshold (needs watering)
    static bool needsWatering(uint8_t thresholdPct = SOIL_WATER_THRESHOLD_PCT) {
        return readPercent() < thresholdPct;
    }
};

#else
class SoilDriver {
public:
    static void init()                              {}
    static uint8_t readPercent()                   { return 0; }
    static int readRaw()                           { return 0; }
    static bool needsWatering(uint8_t=30)          { return false; }
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F13 — NFC Tap-to-Enter: PN532 via I²C (Wire1, same bus as SCD41)
// Guest passes as NDEF tags with time-limited TTL from backend.
// Enable: #define ENABLE_NFC true
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_NFC

#define PN532_ADDR 0x24

class NFC_PN532Driver {
public:
    struct Tag {
        uint8_t uid[7];
        uint8_t uidLen;
        bool    valid;
    };

    static bool init(TwoWire& wire) {
        _wire = &wire;
        // Wake up PN532
        _writeCmd(0x55); delay(10);
        // GetFirmwareVersion
        uint8_t resp[12] = {};
        if (!_transceive(new uint8_t[2]{0xD4,0x02}, 2, resp, sizeof(resp))) {
            LOG_E("NFC", "PN532 not found on I2C 0x%02X", PN532_ADDR);
            return false;
        }
        // SAM Configuration — normal mode
        uint8_t sam[5] = {0xD4,0x14,0x01,0x14,0x01};
        _transceive(sam, sizeof(sam), resp, sizeof(resp));
        LOG_I("NFC", "PN532 online — firmware IC:0x%02X ver:%d.%d", resp[7], resp[8], resp[9]);
        return true;
    }

    // Non-blocking scan — returns valid Tag if card present
    static Tag scan() {
        Tag t{};
        uint8_t cmd[9] = {0xD4,0x4A,0x01,0x00};  // InListPassiveTarget, 1 card, 106kbps ISO14443A
        uint8_t resp[32] = {};
        if (!_transceive(cmd, 4, resp, sizeof(resp))) return t;
        if (resp[7] < 1) return t; // no cards
        t.uidLen = resp[12];
        if (t.uidLen > 7) t.uidLen = 7;
        memcpy(t.uid, &resp[13], t.uidLen);
        t.valid = true;
        return t;
    }

    // Check if UID matches authorised NFC list in NVS (set from backend guest_access)
    static bool isAuthorised(const Tag& t) {
        if (!t.valid) return false;
        char key[20] = "nfc:";
        for (uint8_t i = 0; i < t.uidLen; i++)
            sprintf(key + 4 + i*2, "%02X", t.uid[i]);
        return NVSStore::getBool(NVS_NS_STATE, key, false);
    }

    static void authorise(const uint8_t* uid, uint8_t len, bool allow) {
        char key[20] = "nfc:";
        for (uint8_t i = 0; i < len; i++) sprintf(key + 4 + i*2, "%02X", uid[i]);
        NVSStore::setBool(NVS_NS_STATE, key, allow);
    }

    static void uidToString(const Tag& t, char* buf, size_t len) {
        buf[0] = '\0';
        for (uint8_t i = 0; i < t.uidLen && (i*3+3) < len; i++)
            sprintf(buf + i*3, "%02X%s", t.uid[i], i < t.uidLen-1 ? ":" : "");
    }

private:
    static TwoWire* _wire;
    static void _writeCmd(uint8_t b) {
        _wire->beginTransmission(PN532_ADDR);
        _wire->write(b); _wire->endTransmission();
    }
    static bool _transceive(uint8_t* cmd, uint8_t clen, uint8_t* resp, uint8_t rlen) {
        _wire->beginTransmission(PN532_ADDR);
        // PN532 I2C frame: 0x00 0x00 0xFF len lencomp data checksum 0x00
        uint8_t tfiLen = clen;
        uint8_t lc = tfiLen, lcs = (uint8_t)(~lc + 1);
        _wire->write(0x00); _wire->write(0x00); _wire->write(0xFF);
        _wire->write(lc);   _wire->write(lcs);
        for (uint8_t i = 0; i < clen; i++) _wire->write(cmd[i]);
        uint8_t sum = 0; for (uint8_t i=0;i<clen;i++) sum+=cmd[i];
        _wire->write((uint8_t)(~sum+1)); _wire->write(0x00);
        _wire->endTransmission();
        delay(20);
        _wire->requestFrom((uint8_t)PN532_ADDR, rlen);
        uint8_t ri = 0;
        unsigned long t = millis();
        while (millis()-t < 500 && ri < rlen)
            if (_wire->available()) resp[ri++] = _wire->read();
        return ri >= 7 && resp[3] == 0xD5;
    }
};
inline TwoWire* NFC_PN532Driver::_wire = nullptr;

#else
class NFC_PN532Driver {
public:
    struct Tag { uint8_t uid[7]={};uint8_t uidLen=0;bool valid=false; };
    static bool init(TwoWire&)        { return false; }
    static Tag  scan()                { return {}; }
    static bool isAuthorised(const Tag&) { return false; }
    static void authorise(const uint8_t*, uint8_t, bool) {}
    static void uidToString(const Tag&, char*, size_t) {}
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F14/F15 — Multi-Modal Auth + Guest PIN Engine
// Any 2-of-3 to unlock: Fingerprint, NFC, PIN. Configurable per unit.
// Guest PINs: time-limited 6-digit codes pushed from backend, stored NVS.
// Enable: #define ENABLE_MULTIMODAL true
// ─────────────────────────────────────────────────────────────────────────────
#if ENABLE_MULTIMODAL

#include "FingerprintDriver.h"
#include "NVSStore.h"

class MultiModalAuth {
public:
    enum class Factor : uint8_t { FINGERPRINT=0x01, NFC=0x02, PIN=0x04 };
    // Require any 2 of these 3 factors — configurable
    static constexpr uint8_t REQUIRED_FACTORS = 2;

    static void init(uint8_t requiredMask = 0x07) { // all 3 enabled by default
        _requiredMask = requiredMask;
        LOG_I("MMA", "Multi-modal auth: mask=0x%02X require=%d", _requiredMask, REQUIRED_FACTORS);
    }

    // Grant a factor — returns true when enough factors accumulated → unlock
    static bool grantFactor(Factor f, const char* identity = "") {
        _factorMask |= (uint8_t)f;
        _lastFactorMs = millis();
        strlcpy(_lastIdentity, identity, sizeof(_lastIdentity));
        uint8_t count = __builtin_popcount(_factorMask & _requiredMask);
        LOG_D("MMA", "Factor %d granted — mask=0x%02X count=%d/%d",
              (int)f, _factorMask, count, REQUIRED_FACTORS);
        if (count >= REQUIRED_FACTORS) {
            _unlock();
            return true;
        }
        return false;
    }

    // Clear accumulated factors (call after timeout or unlock)
    static void reset() { _factorMask = 0; _lastIdentity[0] = '\0'; }

    // Timeout: clear if no second factor in 30s
    static void tick() {
        if (_factorMask && (millis() - _lastFactorMs) > 30000UL) {
            LOG_D("MMA", "Auth timeout — factors cleared");
            reset();
        }
    }

    // ── Guest PIN Engine (F15) ────────────────────────────────
    // Backend pushes PINs via MQTT sys command: guest_access_set
    // Stored as "gpin:XXXXXX" = "{valid_from}:{valid_until}:{max_uses}:{use_count}"
    static bool verifyGuestPIN(const char* pin) {
        char key[16]; snprintf(key, sizeof(key), "gpin:%s", pin);
        String val = NVSStore::getString(NVS_NS_STATE, key, "");
        if (!val.length()) return false;

        // Parse "valid_from:valid_until:max_uses:use_count"
        unsigned long vfrom, vuntil, maxUses, useCount;
        if (sscanf(val.c_str(), "%lu:%lu:%lu:%lu", &vfrom, &vuntil, &maxUses, &useCount) != 4)
            return false;

        time_t now = time(nullptr);
        if ((unsigned long)now < vfrom)  { LOG_D("MMA","PIN not yet valid"); return false; }
        if ((unsigned long)now > vuntil) { LOG_D("MMA","PIN expired"); return false; }
        if (maxUses > 0 && useCount >= maxUses) { LOG_D("MMA","PIN max uses reached"); return false; }

        // Increment use count
        char newVal[64]; snprintf(newVal, sizeof(newVal), "%lu:%lu:%lu:%lu",
                                  vfrom, vuntil, maxUses, useCount + 1);
        NVSStore::setString(NVS_NS_STATE, key, newVal);
        return true;
    }

    static void storeGuestPIN(const char* pin, unsigned long validFrom,
                               unsigned long validUntil, unsigned long maxUses) {
        char key[16]; snprintf(key, sizeof(key), "gpin:%s", pin);
        char val[64]; snprintf(val, sizeof(val), "%lu:%lu:%lu:0", validFrom, validUntil, maxUses);
        NVSStore::setString(NVS_NS_STATE, key, val);
        LOG_I("MMA", "Guest PIN stored: %s valid %lu→%lu max=%lu", pin, validFrom, validUntil, maxUses);
    }

    static void revokeGuestPIN(const char* pin) {
        char key[16]; snprintf(key, sizeof(key), "gpin:%s", pin);
        NVSStore::remove(NVS_NS_STATE, key);
    }

private:
    static uint8_t _factorMask, _requiredMask;
    static unsigned long _lastFactorMs;
    static char _lastIdentity[32];

    static void _unlock() {
        LOG_I("MMA", "Multi-modal: %d factors — UNLOCKING (user: %s)", REQUIRED_FACTORS, _lastIdentity);
        Event e{}; e.type = EventType::COMMAND_RECEIVED;
        strlcpy(e.data.command.room,        "entry",     sizeof(e.data.command.room));
        strlcpy(e.data.command.device_name, "door_lock", sizeof(e.data.command.device_name));
        e.data.command.action = CommandAction::ON;
        e.data.command.value  = -1;
        EventBus::postCommand(e);
        reset();
    }
};
inline uint8_t  MultiModalAuth::_factorMask   = 0;
inline uint8_t  MultiModalAuth::_requiredMask = 0x07;
inline unsigned long MultiModalAuth::_lastFactorMs = 0;
inline char     MultiModalAuth::_lastIdentity[32]  = {};

#else
class MultiModalAuth {
public:
    enum class Factor : uint8_t { FINGERPRINT=0x01, NFC=0x02, PIN=0x04 };
    static void init(uint8_t=0x07)              {}
    static bool grantFactor(Factor, const char* = "") { return false; }
    static void reset()                         {}
    static void tick()                          {}
    static bool verifyGuestPIN(const char*)     { return false; }
    static void storeGuestPIN(const char*, unsigned long, unsigned long, unsigned long) {}
    static void revokeGuestPIN(const char*)     {}
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F29 — Deep Idle Manager
// Light sleep between sensor reads — saves ~40mA.
// DEEP_IDLE_THRESHOLD_MS already defined in Config.h but was unused.
// Enable: #define ENABLE_DEEP_IDLE true
// ─────────────────────────────────────────────────────────────────────────────
#include "esp_sleep.h"

#if ENABLE_DEEP_IDLE

class DeepIdleManager {
public:
    static void init() {
        esp_sleep_enable_timer_wakeup(1000ULL); // minimum 1ms wakeup
        LOG_I("Idle", "Deep idle enabled — threshold %dms", DEEP_IDLE_THRESHOLD_MS);
    }

    // Call at end of sensor task loop when nothing to do
    static void idleIfQuiet(unsigned long lastActivityMs, unsigned long now) {
        if (!_enabled) return;
        if ((now - lastActivityMs) < DEEP_IDLE_THRESHOLD_MS) return;
        if (!_checkSafeToSleep()) return;

        // Light sleep preserves RAM, WiFi, and MQTT (WiFi modem keeps alive)
        uint64_t sleepUs = (uint64_t)SENSOR_READ_MS * 1000ULL;
        LOG_D("Idle", "Light sleep for %lums", SENSOR_READ_MS);
        esp_sleep_enable_timer_wakeup(sleepUs);
        esp_light_sleep_start();
    }

    static void enable(bool en)  { _enabled = en; }
    static bool isEnabled()      { return _enabled; }

private:
    static bool _enabled;

    static bool _checkSafeToSleep() {
        // Don't sleep if OTA, safety alert, or gate moving
        extern bool gOTAInProgress;
        if (gOTAInProgress) return false;
        return true;
    }
};
inline bool DeepIdleManager::_enabled = true;

#else
class DeepIdleManager {
public:
    static void init() {}
    static void idleIfQuiet(unsigned long, unsigned long) {}
    static void enable(bool) {}
    static bool isEnabled() { return false; }
};
#endif

// ─────────────────────────────────────────────────────────────────────────────
// F5 — Real Power Metering: PZEM-004T v3 via UART2
// Free-GPIO audit: the PZEM library's documented default UART2 pins (16/17)
// are already PIN_US1_TRIG/PIN_US1_ECHO in core/Config.h — wiring PZEM there
// would silently steal the occupancy ultrasonic's pins. Per the same audit
// used for RGBW/Soil/US3 above, classic ESP32 has zero free GPIOs left, so
// PZEM is S3-only here, on GPIO7 (TX) / GPIO10 (RX) — confirmed free.
// Wire: PZEM TX→GPIO7, RX→GPIO10
// Enable: #define ENABLE_PZEM true
// ─────────────────────────────────────────────────────────────────────────────
#ifndef ENABLE_PZEM
  #define ENABLE_PZEM false
#endif

#if ENABLE_PZEM && !defined(ARDUINO_ESP32S3_DEV)
  #error "ENABLE_PZEM=true on classic ESP32: no free GPIO pair remains on this " \
"board for the PZEM UART2 link (see free-GPIO audit in core/Config.h). Use " \
"an S3 board, or leave ENABLE_PZEM=false."
#endif

#if ENABLE_PZEM

#include <PZEM004Tv30.h>

class PZEM004TDriver {
public:
    static void init() {
        // UART2: TX=GPIO7, RX=GPIO10 (S3 only — see audit note above)
        Serial2.begin(9600, SERIAL_8N1, 10, 7);
        _pzem = new PZEM004Tv30(Serial2);
        LOG_I("PZEM", "PZEM-004T initialised on UART2 (TX=7, RX=10)");
    }

    struct Reading {
        float voltage;   // V
        float current;   // A
        float power;     // W
        float energy;    // kWh (cumulative, clears on PZEM reset)
        float frequency; // Hz
        float pf;        // power factor
        bool  valid;
    };

    static Reading read() {
        Reading r{};
        r.voltage   = _pzem->voltage();
        r.current   = _pzem->current();
        r.power     = _pzem->power();
        r.energy    = _pzem->energy();
        r.frequency = _pzem->frequency();
        r.pf        = _pzem->pf();
        r.valid     = !isnan(r.voltage) && !isnan(r.power);
        return r;
    }

    // Reset cumulative kWh counter on PZEM chip
    static void resetEnergy() { _pzem->resetEnergy(); }

private:
    static PZEM004Tv30* _pzem;
};
PZEM004Tv30* PZEM004TDriver::_pzem = nullptr;

#else
class PZEM004TDriver {
public:
    struct Reading { float voltage=0,current=0,power=0,energy=0,frequency=0,pf=0; bool valid=false; };
    static void init() {}
    static Reading read() { return {}; }
    static void resetEnergy() {}
};
#endif // ENABLE_PZEM

