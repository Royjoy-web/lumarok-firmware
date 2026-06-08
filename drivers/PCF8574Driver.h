#pragma once
#include <Wire.h>
#include "../core/Config.h"

#if USE_I2C_EXPANDER

class PCF8574Driver {
public:
    static bool init() {
        Wire.begin();
        return ping();
    }

    // Returns true if device responds on I2C bus
    static bool ping() {
        Wire.beginTransmission(PCF8574_ADDR);
        return (Wire.endTransmission() == 0);
    }

    // Read all 8 pins as a byte (1 = HIGH)
    static uint8_t readAll() {
        if (Wire.requestFrom(PCF8574_ADDR, (uint8_t)1) == 1) {
            return Wire.read();
        }
        _errors++;
        return 0xFF; // fail-safe = all high (open contacts)
    }

    // Read a single pin (0–7)
    static bool readPin(uint8_t pin) {
        return (readAll() >> pin) & 0x01;
    }

    static bool isDoorOpen()   { return !readPin(PCF8574_PIN_DOOR);   }
    static bool isWindowOpen() { return !readPin(PCF8574_PIN_WINDOW); }

    static uint16_t errorCount() { return _errors; }

    // Attempt I2C bus recovery (clock stretching fix)
    static bool recover() {
        Wire.end();
        delay(10);
        Wire.begin();
        return ping();
    }

private:
    static uint16_t _errors;
};

inline uint16_t PCF8574Driver::_errors = 0;

#else
// Stub for boards without I2C expander
class PCF8574Driver {
public:
    static bool init() {
        pinMode(PIN_DOOR_SENSOR,   INPUT_PULLUP);
        pinMode(PIN_WINDOW_SENSOR, INPUT_PULLUP);
        return true;
    }
    static bool isDoorOpen()   { return digitalRead(PIN_DOOR_SENSOR)   == HIGH; }
    static bool isWindowOpen() { return digitalRead(PIN_WINDOW_SENSOR) == HIGH; }
    static bool ping()         { return true; }
    static bool recover()      { return true; }
    static uint16_t errorCount() { return 0; }
};
#endif
