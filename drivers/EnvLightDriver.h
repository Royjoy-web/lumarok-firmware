#pragma once
#include "../core/Config.h"

// LDR (ambient light) + thermistor (analog temp) on GPIO37/38.
// Raw ADC only — calibrate _ldrToLux/_thermToC constants to your specific
// parts (LDR divider resistor, thermistor beta/R25) before relying on the
// converted values; raw() is always safe to use as-is.
class EnvLightDriver {
public:
    static void init() {
        pinMode(PIN_LDR, INPUT);
        pinMode(PIN_THERMISTOR, INPUT);
    }

    static int ldrRaw()        { return analogRead(PIN_LDR); }        // 0-4095
    static int thermistorRaw() { return analogRead(PIN_THERMISTOR); } // 0-4095

    // Rough light level: 0 (dark) - 100 (bright). Uncalibrated default
    // assumes a 10k pulldown LDR divider; verify against your wiring.
    static float lightPercent() {
        return (ldrRaw() / 4095.0f) * 100.0f;
    }

    // Rough Celsius via simplified NTC beta equation. PLACEHOLDER constants
    // (R25=10k, beta=3950, series=10k) — replace with your thermistor's
    // datasheet values before trusting this for anything beyond on/off logic.
    static float thermistorC() {
        const float seriesR = 10000.0f, r25 = 10000.0f, beta = 3950.0f, t25 = 298.15f;
        int raw = thermistorRaw();
        if (raw <= 0) return NAN;
        float r = seriesR * (4095.0f / raw - 1.0f);
        float k = 1.0f / (1.0f / t25 + log(r / r25) / beta);
        return k - 273.15f;
    }
};
