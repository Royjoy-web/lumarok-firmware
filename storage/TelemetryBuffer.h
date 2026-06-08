#pragma once
#include <Arduino.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../storage/NVSStore.h"

#define TELEM_BUF_SIZE    64

// NVS keys for ring buffer state
#define NVS_KEY_BUF_HEAD  "buf_head"
#define NVS_KEY_BUF_TAIL  "buf_tail"

// Serialised SensorReading entry key: "b00" .. "b63"
static inline String _bufKey(uint8_t idx) {
    char k[4]; snprintf(k, sizeof(k), "b%02d", idx % TELEM_BUF_SIZE);
    return String(k);
}

class TelemetryBuffer {
public:
    static void init() {
        _head = NVSStore::getU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_HEAD, 0);
        _tail = NVSStore::getU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_TAIL, 0);
        _count = (_tail >= _head) ? (_tail - _head)
                                  : (TELEM_BUF_SIZE - _head + _tail);
        LOG_I("TelBuf", "Restored %d pending readings", (int)_count);
    }

    static bool push(const SensorReading& r) {
        if (_count >= TELEM_BUF_SIZE) {
            // Drop oldest QoS-0 style: advance head
            _head = (_head + 1) % TELEM_BUF_SIZE;
            _count--;
            NVSStore::putU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_HEAD, _head);
        }
        NVSStore::putBlob(NVS_NS_TELEMETRY, _bufKey(_tail).c_str(),
                          &r, sizeof(SensorReading));
        _tail = (_tail + 1) % TELEM_BUF_SIZE;
        _count++;
        NVSStore::putU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_TAIL, _tail);
        return true;
    }

    static bool peek(SensorReading& out) {
        if (_count == 0) return false;
        return NVSStore::getBlob(NVS_NS_TELEMETRY, _bufKey(_head).c_str(),
                                 &out, sizeof(SensorReading)) == sizeof(SensorReading);
    }

    static bool pop() {
        if (_count == 0) return false;
        NVSStore::remove(NVS_NS_TELEMETRY, _bufKey(_head).c_str());
        _head = (_head + 1) % TELEM_BUF_SIZE;
        _count--;
        NVSStore::putU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_HEAD, _head);
        return true;
    }

    static uint8_t count()   { return _count; }
    static bool    isEmpty() { return _count == 0; }
    static bool    isFull()  { return _count >= TELEM_BUF_SIZE; }

    static void clear() {
        _head = 0; _tail = 0; _count = 0;
        NVSStore::putU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_HEAD, 0);
        NVSStore::putU8(NVS_NS_TELEMETRY, NVS_KEY_BUF_TAIL, 0);
    }

private:
    static uint8_t _head;
    static uint8_t _tail;
    static uint8_t _count;
};

inline uint8_t TelemetryBuffer::_head  = 0;
inline uint8_t TelemetryBuffer::_tail  = 0;
inline uint8_t TelemetryBuffer::_count = 0;
