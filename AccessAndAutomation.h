#pragma once
#include <Arduino.h>

// Minimal multi-channel PWM dimmer over ESP32 ledc, used by RGBWDriver
// (ENABLE_RGBW). Channels 4-7 reserved here — channels 1-3 are already used
// by USE_RGB in hal/RelayHAL.h, channel 0 reserved for future use.
class PWMDimmer {
public:
    // Registers the next free channel (4-7) on the given pin. Returns -1 if
    // all 4 RGBW channels are already taken.
    static int registerChannel(int pin) {
        if (_next > 7) return -1;
        int ch = _next++;
        ledcSetup(ch, 5000, 8);   // 5kHz, 8-bit (0-255)
        ledcAttachPin(pin, ch);
        return ch;
    }

    // value: 0-100 (%), converted to 0-255 duty internally.
    static void setBrightness(int channel, int value) {
        if (channel < 0) return;
        int duty = constrain(value, 0, 100) * 255 / 100;
        ledcWrite(channel, duty);
    }

private:
    static inline int _next = 4;
};
