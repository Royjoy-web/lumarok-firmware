#pragma once
#include <Arduino.h>
#include "../core/Types.h"
#include "../core/Config.h"

// RelayHAL — pure GPIO relay driver.
// Knows nothing about MQTT, devices, or business logic.
// All state tracking lives in DeviceRegistry; HAL is stateless per-pin.
class RelayHAL {
public:
    static void init(uint8_t pin, bool activeLow = false) {
        pinMode(pin, OUTPUT);
        // Default to safe-off state (relay open)
        digitalWrite(pin, activeLow ? HIGH : LOW);
    }

    // Low-level pin write — no side-effects
    static void set(uint8_t pin, bool on, bool activeLow = false) {
        digitalWrite(pin, activeLow ? !on : on);
    }

    static bool get(uint8_t pin) {
        return digitalRead(pin) == HIGH;
    }

    static void toggle(uint8_t pin, bool activeLow = false) {
        bool current = get(pin);
        set(pin, !current, activeLow);
    }

    // Convenience: turn all listed pins off (called in safe-shutdown)
    static void setAll(const uint8_t* pins, uint8_t count, bool on) {
        for (uint8_t i = 0; i < count; i++) set(pins[i], on);
    }
};

// ── Servo HAL ─────────────────────────────────────────────────
#include <ESP32Servo.h>

class ServoHAL {
public:
    static void init() {
        _servo.setPeriodHertz(SERVO_FREQ_HZ);
        _servo.attach(PIN_SERVO_BLINDS, SERVO_US_MIN, SERVO_US_MAX);
        _position = 90;
        _servo.write(90);
    }

    // position: 0–180 degrees
    static void setPosition(int pos) {
        pos = constrain(pos, 0, 180);
        _servo.write(pos);
        _position = pos;
    }

    static int getPosition() { return _position; }

private:
    static Servo _servo;
    static int   _position;
};

inline Servo ServoHAL::_servo;
inline int   ServoHAL::_position = 90;

// ── RGBLED HAL ────────────────────────────────────────────────
#if USE_RGB
class RGBHAL {
public:
    enum Color { OFF=0, RED, GREEN, BLUE, YELLOW, CYAN, WHITE, PURPLE };

    static void init() {
        ledcSetup(1, 5000, 8); ledcAttachPin(PIN_RGB_R, 1);
        ledcSetup(2, 5000, 8); ledcAttachPin(PIN_RGB_G, 2);
        ledcSetup(3, 5000, 8); ledcAttachPin(PIN_RGB_B, 3);
        set(OFF);
    }

    static void set(Color c) {
        uint8_t r=0, g=0, b=0;
        switch(c) {
            case RED:    r=255;          break;
            case GREEN:          g=255;  break;
            case BLUE:                   b=255; break;
            case YELLOW: r=255; g=255;   break;
            case CYAN:           g=255;  b=255; break;
            case WHITE:  r=255; g=255;   b=255; break;
            case PURPLE: r=128;          b=255; break;
            default:                     break;
        }
        ledcWrite(1, r); ledcWrite(2, g); ledcWrite(3, b);
    }
};
#else
class RGBHAL {
public:
    enum Color { OFF=0, RED, GREEN, BLUE, YELLOW, CYAN, WHITE, PURPLE };
    static void init() {}
    static void set(Color) {}
};
#endif
