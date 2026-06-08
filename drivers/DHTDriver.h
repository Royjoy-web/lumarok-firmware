#pragma once
#include <DHT.h>
#include "../core/Types.h"
#include "../core/Config.h"

struct DHTReading {
    float    temperature;
    float    humidity;
    bool     valid;
    uint16_t errorCount;
    uint16_t consecutiveErrors;
};

class DHTDriver {
public:
    static void init() {
        _dht.begin();
        LOG_I("DHT", "DHT22 ready on pin %d", PIN_DHT_SENSOR);
    }

    static DHTReading read() {
        DHTReading r{};
        float h = _dht.readHumidity();
        float t = _dht.readTemperature();

        if (isnan(h) || isnan(t)) {
            _errorCount++;
            _consecutiveErrors++;
            r.valid             = false;
            r.errorCount        = _errorCount;
            r.consecutiveErrors = _consecutiveErrors;
            LOG_W("DHT", "Read failed (total errors: %d)", _errorCount);
            return r;
        }

        _consecutiveErrors = 0;
        r.temperature       = t;
        r.humidity          = h;
        r.valid             = true;
        r.errorCount        = _errorCount;
        r.consecutiveErrors = 0;
        return r;
    }

    static uint16_t totalErrors()       { return _errorCount; }
    static uint16_t consecutiveErrors() { return _consecutiveErrors; }

    // Sensor degraded if > 5 consecutive failures
    static bool isDegraded() { return _consecutiveErrors > 5; }

private:
    static DHT    _dht;
    static uint16_t _errorCount;
    static uint16_t _consecutiveErrors;
};

inline DHT     DHTDriver::_dht(PIN_DHT_SENSOR, DHT22);
inline uint16_t DHTDriver::_errorCount        = 0;
inline uint16_t DHTDriver::_consecutiveErrors = 0;
