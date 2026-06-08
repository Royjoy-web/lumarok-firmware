#pragma once
#include <Arduino.h>
#include "../core/Config.h"
#include "../core/Types.h"

struct GasReading {
    int  analog;       // raw ADC value (0–4095)
    bool digitalAlert; // MQ module onboard comparator output
    bool aboveThreshold;
    bool valid;
};

class GasDriver {
public:
    static void init() {
        pinMode(PIN_GAS_ANALOG,  INPUT);
        pinMode(PIN_GAS_DIGITAL, INPUT);
        LOG_I("Gas", "MQ gas sensor ready (analog=%d digital=%d)",
              PIN_GAS_ANALOG, PIN_GAS_DIGITAL);
    }

    static GasReading read() {
        GasReading r{};
        r.analog          = analogRead(PIN_GAS_ANALOG);
        r.digitalAlert    = (digitalRead(PIN_GAS_DIGITAL) == HIGH);
        r.aboveThreshold  = (r.analog >= GAS_ALERT_THRESHOLD) || r.digitalAlert;
        r.valid           = true;
        return r;
    }

    // Returns ppm-approximation (linear interpolation; calibrate per sensor)
    static float toPPM(int raw) {
        // Rough conversion for LPG: adjust Rs/R0 per datasheet
        return (float)raw * 0.5f;
    }
};
