#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include "../storage/NVSStore.h"
#include "../core/Types.h"

// ─────────────────────────────────────────────────────────────
// FlashWearGuard — coalesces NVS writes to reduce flash wear.
//
// Problem: repeated small writes (device state on every toggle,
// crash counter on every boot, ring-buffer head/tail on every
// sensor read) erode NVS flash sectors quickly.
//
// Solution: pending writes are held in RAM and flushed in a
// single NVS transaction after a quiet period (FLUSH_DELAY_MS),
// or immediately for safety-critical keys.
//
// Flash write budget: tracks writes per 24h window and warns
// when approaching NVS sector endurance (~10,000 erase cycles).
// ─────────────────────────────────────────────────────────────

#define FLUSH_DELAY_MS        2000UL   // coalesce window
#define MAX_DIRTY_ENTRIES     24
#define WRITE_BUDGET_PER_DAY  500      // warn if exceeded
#define NVS_KEY_WRITE_COUNT   "nwc"    // persisted write counter

struct DirtyEntry {
    char     ns[16];
    char     key[16];
    char     strVal[64];
    uint32_t u32Val;
    uint8_t  u8Val;
    bool     boolVal;
    enum class T : uint8_t { NONE, STR, U32, U8, BOOL } type = T::NONE;
};

class FlashWearGuard {
public:
    static void init() {
        _mutex      = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        _writeCount = NVSStore::getU32(NVS_NS_DIAG, NVS_KEY_WRITE_COUNT, 0);
        _dayStartMs = millis();
        _timer = xTimerCreate("NVSFlush", pdMS_TO_TICKS(FLUSH_DELAY_MS),
                               pdFALSE, nullptr, _flushCb);
        configASSERT(_timer);
        LOG_I("Flash", "NVS write count (lifetime): %u", (unsigned)_writeCount);
    }

    // ── Coalesced write APIs ──────────────────────────────────
    static void putString(const char* ns, const char* key, const char* val,
                           bool immediate = false) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        DirtyEntry* e = _findOrCreate(ns, key);
        if (!e) { xSemaphoreGive(_mutex); NVSStore::putString(ns, key, val); return; }
        e->type = DirtyEntry::T::STR;
        strlcpy(e->strVal, val, sizeof(e->strVal));
        xSemaphoreGive(_mutex);
        immediate ? _flush() : _arm();
    }

    static void putU32(const char* ns, const char* key, uint32_t val,
                        bool immediate = false) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        DirtyEntry* e = _findOrCreate(ns, key);
        if (!e) { xSemaphoreGive(_mutex); NVSStore::putU32(ns, key, val); return; }
        e->type   = DirtyEntry::T::U32;
        e->u32Val = val;
        xSemaphoreGive(_mutex);
        immediate ? _flush() : _arm();
    }

    static void putU8(const char* ns, const char* key, uint8_t val,
                       bool immediate = false) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        DirtyEntry* e = _findOrCreate(ns, key);
        if (!e) { xSemaphoreGive(_mutex); NVSStore::putU8(ns, key, val); return; }
        e->type  = DirtyEntry::T::U8;
        e->u8Val = val;
        xSemaphoreGive(_mutex);
        immediate ? _flush() : _arm();
    }

    static void putBool(const char* ns, const char* key, bool val,
                         bool immediate = false) {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        DirtyEntry* e = _findOrCreate(ns, key);
        if (!e) { xSemaphoreGive(_mutex); NVSStore::putBool(ns, key, val); return; }
        e->type    = DirtyEntry::T::BOOL;
        e->boolVal = val;
        xSemaphoreGive(_mutex);
        immediate ? _flush() : _arm();
    }

    static void flushAll() { _flush(); }

    static uint32_t lifetimeWrites()  { return _writeCount; }
    static uint8_t  dirtyCount()      { return _dirtyCount; }

    // Daily budget check — call from DiagnosticsTask
    static void checkBudget() {
        uint32_t elapsed = millis() - _dayStartMs;
        if (elapsed >= 86400000UL) {
            if (_dailyWrites > WRITE_BUDGET_PER_DAY) {
                LOG_W("Flash", "Write budget exceeded: %u writes/day (limit %d)",
                      (unsigned)_dailyWrites, WRITE_BUDGET_PER_DAY);
            }
            _dailyWrites = 0;
            _dayStartMs  = millis();
        }
    }

private:
    static DirtyEntry* _findOrCreate(const char* ns, const char* key) {
        for (uint8_t i = 0; i < _dirtyCount; i++) {
            if (strcmp(_dirty[i].ns, ns) == 0 &&
                strcmp(_dirty[i].key, key) == 0)
                return &_dirty[i];
        }
        if (_dirtyCount >= MAX_DIRTY_ENTRIES) return nullptr;
        DirtyEntry* e = &_dirty[_dirtyCount++];
        strlcpy(e->ns,  ns,  sizeof(e->ns));
        strlcpy(e->key, key, sizeof(e->key));
        return e;
    }

    static void _arm() {
        xTimerReset(_timer, pdMS_TO_TICKS(10));
    }

    static void _flushCb(TimerHandle_t) { _flush(); }

    static void _flush() {
        xSemaphoreTake(_mutex, portMAX_DELAY);
        if (_dirtyCount == 0) { xSemaphoreGive(_mutex); return; }
        // Snapshot dirty list under lock, then release before NVS writes
        DirtyEntry snapshot[MAX_DIRTY_ENTRIES];
        uint8_t cnt = _dirtyCount;
        memcpy(snapshot, _dirty, cnt * sizeof(DirtyEntry));
        _dirtyCount = 0;
        xSemaphoreGive(_mutex);

        // NVS writes happen outside the lock — they can block and we don't
        // want to hold the mutex across slow flash operations.
        for (uint8_t i = 0; i < cnt; i++) {
            DirtyEntry& e = snapshot[i];
            switch (e.type) {
                case DirtyEntry::T::STR:  NVSStore::putString(e.ns, e.key, e.strVal); break;
                case DirtyEntry::T::U32:  NVSStore::putU32   (e.ns, e.key, e.u32Val); break;
                case DirtyEntry::T::U8:   NVSStore::putU8    (e.ns, e.key, e.u8Val);  break;
                case DirtyEntry::T::BOOL: NVSStore::putBool  (e.ns, e.key, e.boolVal);break;
                default: break;
            }
        }
        _writeCount  += cnt;
        _dailyWrites += cnt;
        if (_writeCount % 100 == 0)
            NVSStore::putU32(NVS_NS_DIAG, NVS_KEY_WRITE_COUNT, _writeCount);
        LOG_D("Flash", "Flushed %u entries — lifetime: %u", (unsigned)cnt, (unsigned)_writeCount);
    }

    static DirtyEntry    _dirty[MAX_DIRTY_ENTRIES];
    static uint8_t       _dirtyCount;
    static TimerHandle_t _timer;
    static SemaphoreHandle_t _mutex;
    static uint32_t      _writeCount;
    static uint32_t      _dailyWrites;
    static unsigned long _dayStartMs;
};

inline DirtyEntry    FlashWearGuard::_dirty[MAX_DIRTY_ENTRIES] = {};
inline uint8_t       FlashWearGuard::_dirtyCount    = 0;
inline TimerHandle_t FlashWearGuard::_timer         = nullptr;
inline SemaphoreHandle_t FlashWearGuard::_mutex     = nullptr;
inline uint32_t      FlashWearGuard::_writeCount    = 0;
inline uint32_t      FlashWearGuard::_dailyWrites   = 0;
inline unsigned long FlashWearGuard::_dayStartMs    = 0;
