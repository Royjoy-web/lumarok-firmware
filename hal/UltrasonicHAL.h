#pragma once
#include <Arduino.h>
#include "../core/Config.h"

class UltrasonicHAL {
public:
    static void init(uint8_t trigPin, uint8_t echoPin) {
        pinMode(trigPin, OUTPUT);
        pinMode(echoPin, INPUT);
        digitalWrite(trigPin, LOW);
    }

    // Returns distance in cm, or -1.0f on timeout
    static float readCm(uint8_t trigPin, uint8_t echoPin,
                         unsigned long timeoutUs = 30000UL) {
        // Ensure clean trigger
        digitalWrite(trigPin, LOW);
        delayMicroseconds(2);
        digitalWrite(trigPin, HIGH);
        delayMicroseconds(10);
        digitalWrite(trigPin, LOW);

        long duration = pulseIn(echoPin, HIGH, timeoutUs);
        if (duration == 0) return -1.0f;   // timeout
        return (duration * 0.034f) / 2.0f;
    }

    // Multi-sample median (reduces spurious readings)
    static float readMedianCm(uint8_t trigPin, uint8_t echoPin,
                               uint8_t samples = 3) {
        float buf[5];
        uint8_t n = constrain(samples, 1, 5);
        for (uint8_t i = 0; i < n; i++) {
            buf[i] = readCm(trigPin, echoPin);
            delay(20);
        }
        // Simple insertion sort
        for (uint8_t i = 1; i < n; i++) {
            float key = buf[i]; int j = i - 1;
            while (j >= 0 && buf[j] > key) { buf[j+1] = buf[j]; j--; }
            buf[j+1] = key;
        }
        return buf[n / 2];
    }
};
