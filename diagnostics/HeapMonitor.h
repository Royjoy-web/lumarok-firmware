#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include "../core/Types.h"
#include "../core/Config.h"

// ── HeapMonitor ───────────────────────────────────────────────
// Tracks free heap, minimum ever seen, largest contiguous block,
// and fragmentation ratio. Triggers controlled restart if critical.
class HeapMonitor {
public:
    static constexpr uint32_t WARN_BYTES     = 30000;
    static constexpr uint32_t CRITICAL_BYTES = 15000;
    // BUG-9: FaultManager triggers esp_restart() at HEAP_FREE_CRIT_BYTES (60 KB),
    // well above these thresholds. These paths are dead code — the device restarts
    // before reaching them. Assert to document this and catch future threshold drift.
    static_assert(CRITICAL_BYTES < 60000U,
        "HeapMonitor::CRITICAL_BYTES is above FaultManager's HEAP_FREE_CRIT_BYTES (60 KB). "
        "HeapMonitor restart path is unreachable. Update HeapMonitor constants or remove them.");

    static void tick() {
        _free    = ESP.getFreeHeap();
        _minFree = ESP.getMinFreeHeap();
        _largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

        // Fragmentation: 1.0 = perfect, <0.5 = badly fragmented
        _fragRatio = (_free > 0) ? ((float)_largest / (float)_free) : 1.0f;

        if (_free < CRITICAL_BYTES && !_critFired) {
            _critFired = true;
            LOG_E("Heap", "CRITICAL %u free  frag=%.2f — restarting", _free, _fragRatio);
            vTaskDelay(pdMS_TO_TICKS(300));
            esp_restart();
        }
        if (_free < WARN_BYTES && !_warnFired) {
            _warnFired = true;
            LOG_W("Heap", "LOW %u free  min=%u  largest=%u  frag=%.2f",
                  _free, _minFree, _largest, _fragRatio);
        }
        if (_free > WARN_BYTES + 8000) {
            _warnFired = false;
            _critFired = false;
        }
    }

    static uint32_t free()       { return _free;      }
    static uint32_t minFree()    { return _minFree;    }
    static uint32_t largest()    { return _largest;    }
    static float    fragRatio()  { return _fragRatio;  }
    static bool     isLow()      { return _free < WARN_BYTES;     }
    static bool     isCritical() { return _free < CRITICAL_BYTES; }

private:
    static uint32_t _free;
    static uint32_t _minFree;
    static uint32_t _largest;
    static float    _fragRatio;
    static bool     _warnFired;
    static bool     _critFired;
};

inline uint32_t HeapMonitor::_free      = 0;
inline uint32_t HeapMonitor::_minFree   = 0;
inline uint32_t HeapMonitor::_largest   = 0;
inline float    HeapMonitor::_fragRatio = 1.0f;
inline bool     HeapMonitor::_warnFired = false;
inline bool     HeapMonitor::_critFired = false;
