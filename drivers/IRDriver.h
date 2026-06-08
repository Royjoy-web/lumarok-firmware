#pragma once
#include <Arduino.h>
#include "../core/Config.h"

class IRDriver {
public:
    static void init() {
        pinMode(PIN_IR_SENSOR, INPUT);
    }

    // Returns true if beam is broken (obstacle present)
    // Debounced: requires DEBOUNCE_MS stable readings
    static bool isBeamBroken(unsigned long debounceMs = 50) {
        bool current = (digitalRead(PIN_IR_SENSOR) == HIGH);
        if (current != _lastRaw) {
            _lastChangeMs = millis();
            _lastRaw = current;
        }
        if ((millis() - _lastChangeMs) >= debounceMs) {
            _stable = current;
        }
        return _stable;
    }

    static bool rawRead() { return digitalRead(PIN_IR_SENSOR) == HIGH; }

private:
    static bool          _lastRaw;
    static bool          _stable;
    static unsigned long _lastChangeMs;
};

inline bool          IRDriver::_lastRaw      = false;
inline bool          IRDriver::_stable       = false;
inline unsigned long IRDriver::_lastChangeMs = 0;
