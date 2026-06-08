#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/Types.h"
#include "../storage/NVSStore.h"
#include "../core/Config.h"

// ─────────────────────────────────────────────────────────────
// PriorityBuffer — two-tier offline queue.
//
// HIGH  (alerts, state changes)  — 16 slots, never dropped on overflow.
//                                   Spills to NVS when RAM full.
// NORMAL (sensor telemetry)      — 48 slots in RAM ring, overflow drops
//                                   oldest. NVS drain on reconnect.
//
// Separation prevents high-importance events (gas alert, door open)
// from being evicted by continuous sensor telemetry.
// ─────────────────────────────────────────────────────────────

#define PB_HIGH_SLOTS   16
#define PB_NORMAL_SLOTS 48
#define PB_NVS_SLOTS    32      // NVS overflow for HIGH priority only

struct BufferedReading {
    SensorReading reading;
    uint8_t       priority;   // 0=normal, 1=high
    bool          used;
};

class PriorityBuffer {
public:
    static void init() {
        _mutex = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        memset(_highBuf,   0, sizeof(_highBuf));
        memset(_normalBuf, 0, sizeof(_normalBuf));
        _highHead = _highTail = _highCount = 0;
        _normHead = _normTail = _normCount = 0;
        _droppedCount = 0;
        _loadFromNVS();
    }

    // ── Push ─────────────────────────────────────────────────
    static bool push(const SensorReading& r, uint8_t priority = 0) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        bool ok;
        if (priority > 0) ok = _pushHigh(r);
        else               ok = _pushNormal(r);
        xSemaphoreGive(_mutex);
        return ok;
    }

    // ── Peek / pop (highest priority first) ──────────────────
    static bool peek(SensorReading& out, uint8_t& priorityOut) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        bool ok = false;
        if (_highCount > 0) {
            out = _highBuf[_highHead]; priorityOut = 1; ok = true;
        } else if (_normCount > 0) {
            out = _normalBuf[_normHead]; priorityOut = 0; ok = true;
        }
        xSemaphoreGive(_mutex);
        return ok;
    }

    static bool pop() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        bool ok = false;
        if (_highCount > 0) {
            _highHead = (_highHead + 1) % PB_HIGH_SLOTS;
            _highCount--;
            ok = true;
        } else if (_normCount > 0) {
            _normHead = (_normHead + 1) % PB_NORMAL_SLOTS;
            _normCount--;
            ok = true;
        }
        xSemaphoreGive(_mutex);
        return ok;
    }

    static uint16_t totalPending() {
        return (uint16_t)_highCount + (uint16_t)_normCount + (uint16_t)_nvsCount;
    }
    static uint8_t  highPending()   { return _highCount; }
    static uint8_t  normalPending() { return _normCount; }
    static uint8_t  nvsPending()    { return _nvsCount;  }
    static uint32_t dropped()       { return _droppedCount; }
    static bool     isEmpty()       { return totalPending() == 0; }

private:
    static bool _pushHigh(const SensorReading& r) {
        if (_highCount >= PB_HIGH_SLOTS) {
            // Spill to NVS rather than dropping
            spillToNVS(r);
            return true;
        }
        _highBuf[_highTail] = r;
        _highTail = (_highTail + 1) % PB_HIGH_SLOTS;
        _highCount++;
        return true;
    }

    static bool _pushNormal(const SensorReading& r) {
        if (_normCount >= PB_NORMAL_SLOTS) {
            // Drop oldest normal reading (telemetry is expendable)
            _normHead = (_normHead + 1) % PB_NORMAL_SLOTS;
            _normCount--;
            _droppedCount++;
        }
        _normalBuf[_normTail] = r;
        _normTail = (_normTail + 1) % PB_NORMAL_SLOTS;
        _normCount++;
        return true;
    }

    // BUG-B fix: _nvsWriteIdx wraps 0..PB_NVS_SLOTS-1 (ring write cursor).
    // _nvsCount saturates at PB_NVS_SLOTS and is persisted as "hpc".
    // Previously a single _nvsCount was used for both, so after 32 spills
    // it wrapped to 0 and _loadFromNVS() silently discarded all NVS entries.
    static void spillToNVS(const SensorReading& r) {
        char key[8];
        snprintf(key, sizeof(key), "hp%02d", _nvsWriteIdx);
        NVSStore::putBlob(NVS_NS_TELEMETRY, key, &r, sizeof(r));
        _nvsWriteIdx = (_nvsWriteIdx + 1) % PB_NVS_SLOTS;
        if (_nvsCount < PB_NVS_SLOTS) _nvsCount++;
        NVSStore::putU8(NVS_NS_TELEMETRY, "hpc", _nvsCount);
    }

    static void _loadFromNVS() {
        _nvsCount = NVSStore::getU8(NVS_NS_TELEMETRY, "hpc", 0);
        if (_nvsCount > 0)
            LOG_I("PriBuf", "Restored %d high-priority readings from NVS", (int)_nvsCount);
    }

    static SensorReading  _highBuf[PB_HIGH_SLOTS];
    static SensorReading  _normalBuf[PB_NORMAL_SLOTS];
    static uint8_t        _highHead, _highTail, _highCount;
    static uint8_t        _normHead, _normTail, _normCount;
    static uint8_t        _nvsCount;      // number of entries in NVS (saturates at PB_NVS_SLOTS)
    static uint8_t        _nvsWriteIdx;   // ring write cursor (wraps 0..PB_NVS_SLOTS-1)
    static uint32_t       _droppedCount;
    static SemaphoreHandle_t _mutex;
};

inline SensorReading  PriorityBuffer::_highBuf[PB_HIGH_SLOTS]     = {};
inline SensorReading  PriorityBuffer::_normalBuf[PB_NORMAL_SLOTS] = {};
inline uint8_t        PriorityBuffer::_highHead   = 0;
inline uint8_t        PriorityBuffer::_highTail   = 0;
inline uint8_t        PriorityBuffer::_highCount  = 0;
inline uint8_t        PriorityBuffer::_normHead   = 0;
inline uint8_t        PriorityBuffer::_normTail   = 0;
inline uint8_t        PriorityBuffer::_normCount  = 0;
inline uint8_t        PriorityBuffer::_nvsCount   = 0;
inline uint8_t        PriorityBuffer::_nvsWriteIdx= 0;
inline uint32_t       PriorityBuffer::_droppedCount = 0;
inline SemaphoreHandle_t PriorityBuffer::_mutex   = nullptr;
