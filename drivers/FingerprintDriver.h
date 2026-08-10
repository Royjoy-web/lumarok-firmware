#pragma once
// FingerprintDriver.h — AS608/R307 fingerprint sensor via UART
// Library: Adafruit Fingerprint Sensor Library
// Wire: SENSOR_TX→GPIO32, SENSOR_RX→GPIO33 (configurable)
//
// HARDENING (rewrite): the original version called straight into the
// Adafruit library's serial handshake with no readiness tracking. When no
// physical sensor is wired up, that handshake was observed to crash with
// a null-queue FreeRTOS assert (xQueueSemaphoreTake, queue.c:1709) instead
// of failing cleanly — and every public method (authenticate/enroll/
// deleteTemplate/templateCount) blindly dereferenced _fp regardless of
// whether init() had actually succeeded, so any caller (the sensor task's
// scan loop, or CommandDispatcher.h handling an MQTT enroll/delete command)
// could hit the same crash independently. This version:
//   1. Verifies the underlying UART driver actually installed before ever
//      touching the sensor library (uart_is_driver_installed).
//   2. Tracks a real readiness state — every public method checks it first
//      and fails safely (false / -1 / 0) instead of touching a sensor that
//      isn't there.
//   3. Adds a mutex, matching the pattern already used elsewhere in this
//      codebase (BatchPublisher.h, DeviceRegistry.h, etc.) — FingerprintDriver
//      is called from both the sensor task's scan loop and MQTT command
//      handlers on a different task, so concurrent access was previously
//      unprotected.
//   4. Tears down cleanly (deletes and nulls _fp/_serial) on init failure so
//      nothing downstream can accidentally operate on a half-built object.
#include <Arduino.h>
#include <Adafruit_Fingerprint.h>
#include "driver/uart.h"
#include "Config.h"
#include "EventBus.h"

#ifndef PIN_FP_RX
// FIX (pin-audit): old defaults (32/33) collided with PIN_OUTDOOR_SOCKET/
// PIN_OUTDOOR_LIGHT — see Config.h pin-audit note. Config.h now defines
// these centrally and is included above, so this fallback should be dead
// code in practice; kept non-conflicting regardless.
#define PIN_FP_RX 39
#endif
#ifndef PIN_FP_TX
#define PIN_FP_TX 25
#endif
#ifndef FP_MAX_TEMPLATES
#define FP_MAX_TEMPLATES 127
#endif
#ifndef FP_UART_NUM
#define FP_UART_NUM UART_NUM_2   // matches HardwareSerial(2) below
#endif

class FingerprintDriver {
public:
    // Bounded, defensive init — never leaves _fp/_serial half-built, and
    // never lets a failed handshake propagate into a crash. Safe to call
    // even if a previous init() already failed (idempotent).
    static bool init() {
        if (!_mutex) { _mutex = xSemaphoreCreateMutex(); configASSERT(_mutex); }
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
            LOG_E("FP", "init: mutex timeout");
            return false;
        }

        _ready = false;
        _teardown();  // clean slate in case of a prior partial init

        _serial = new HardwareSerial(2);
        _serial->begin(57600, SERIAL_8N1, PIN_FP_RX, PIN_FP_TX);

        // Confirm the UART peripheral actually came up before touching the
        // sensor library at all — this is the check that was missing before,
        // and the most likely explanation for the null-queue crash: calling
        // into a library that reads/writes a UART whose driver queue was
        // never (or only partially) created.
        if (!uart_is_driver_installed(FP_UART_NUM)) {
            LOG_E("FP", "UART%d driver not installed — skipping sensor handshake", (int)FP_UART_NUM);
            _teardown();
            xSemaphoreGive(_mutex);
            return false;
        }

        _serial->setTimeout(750);  // bound every blocking read the library does

        _fp = new Adafruit_Fingerprint(_serial);
        _fp->begin(57600);

        bool found = _fp->verifyPassword();
        if (!found) {
            LOG_E("FP", "Fingerprint sensor not found");
            _teardown();
            xSemaphoreGive(_mutex);
            return false;
        }

        _fp->getParameters();
        LOG_I("FP", "Sensor capacity: %d templates", _fp->capacity);
        _ready = true;
        xSemaphoreGive(_mutex);
        return true;
    }

    static bool isReady() { return _ready; }

    // Returns matched template ID (1-127), or -1 on fail/not-ready.
    static int8_t authenticate(uint8_t tries = 3) {
        if (!_ready) return -1;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return -1;

        int8_t result = -1;
        for (uint8_t t = 0; t < tries && _ready; t++) {
            if (_fp->getImage()         != FINGERPRINT_OK) continue;
            if (_fp->image2Tz()         != FINGERPRINT_OK) continue;
            if (_fp->fingerFastSearch() != FINGERPRINT_OK) continue;
            if (_fp->confidence >= 60) {
                LOG_I("FP", "Match id=%d conf=%d", _fp->fingerID, _fp->confidence);
                _postAlert("fp_match", _fp->fingerID);
                result = (int8_t)_fp->fingerID;
                break;
            }
        }
        if (result < 0) _postAlert("fp_fail", -1);
        xSemaphoreGive(_mutex);
        return result;
    }

    // Enroll a finger into slot id (1-127). Returns true on success/false
    // if not ready or enrollment fails.
    static bool enroll(uint8_t id) {
        if (!_ready) { LOG_W("FP", "enroll(%d) ignored — sensor not ready", id); return false; }
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;

        LOG_I("FP", "Enrolling id=%d — place finger twice", id);
        bool ok = true;
        for (uint8_t slot = 1; slot <= 2 && ok; slot++) {
            uint8_t attempt = 0;
            while (_fp->getImage() != FINGERPRINT_OK && ++attempt < 20) delay(200);
            if (attempt >= 20) { ok = false; break; }
            if (_fp->image2Tz(slot) != FINGERPRINT_OK) { ok = false; break; }
            if (slot == 1) { LOG_I("FP","First capture OK — lift finger"); delay(1500); }
        }
        if (ok && _fp->createModel()  != FINGERPRINT_OK) ok = false;
        if (ok && _fp->storeModel(id) != FINGERPRINT_OK) ok = false;
        if (ok) LOG_I("FP", "Enrolled id=%d", id);

        xSemaphoreGive(_mutex);
        return ok;
    }

    static bool deleteTemplate(uint8_t id) {
        if (!_ready) return false;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return false;
        bool ok = _fp->deleteModel(id) == FINGERPRINT_OK;
        xSemaphoreGive(_mutex);
        return ok;
    }

    static uint16_t templateCount() {
        if (!_ready) return 0;
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) != pdTRUE) return 0;
        _fp->getTemplateCount();
        uint16_t n = _fp->templateCount;
        xSemaphoreGive(_mutex);
        return n;
    }

private:
    static void _postAlert(const char* type, int16_t id) {
        Event e{}; e.type = EventType::SAFETY_ALERT;
        strlcpy(e.data.alert.type, type, sizeof(e.data.alert.type));
        snprintf(e.data.alert.message, sizeof(e.data.alert.message), "id=%d", id);
        e.data.alert.device[0] = '\0';
        e.data.alert.severity  = AlertSeverity::INFO;
        e.data.alert.timestamp = 0;
        EventBus::postAlert(e);
    }

    // Deletes and nulls _fp/_serial — leaves the object in a known, safe,
    // not-ready state. Caller must hold _mutex.
    static void _teardown() {
        if (_fp)     { delete _fp;     _fp     = nullptr; }
        if (_serial) { delete _serial; _serial = nullptr; }
    }

    static HardwareSerial*       _serial;
    static Adafruit_Fingerprint* _fp;
    static SemaphoreHandle_t     _mutex;
    static volatile bool         _ready;
};
HardwareSerial*       FingerprintDriver::_serial = nullptr;
Adafruit_Fingerprint* FingerprintDriver::_fp     = nullptr;
SemaphoreHandle_t     FingerprintDriver::_mutex  = nullptr;
volatile bool         FingerprintDriver::_ready  = false;
