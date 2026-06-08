#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "../core/Types.h"

// ─────────────────────────────────────────────────────────────
// StaticPool<T, N> — fixed-size object pool using a freelist.
// Zero heap allocations after init(). Thread-safe via mutex.
// Use for SensorReading, MQTTPub, Alert — any hot-path struct.
// ─────────────────────────────────────────────────────────────
template<typename T, size_t N>
class StaticPool {
public:
    void init() {
        _mutex = xSemaphoreCreateMutex();
        configASSERT(_mutex);
        for (size_t i = 0; i < N - 1; i++) _next[i] = i + 1;
        _next[N - 1] = NONE;
        _free = 0;
        _used = 0;
        _peakUsed = 0;
    }

    T* acquire() {
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(5)) != pdTRUE) return nullptr;
        if (_free == NONE) { xSemaphoreGive(_mutex); return nullptr; }
        size_t idx = _free;
        _free = _next[idx];
        _used++;
        if (_used > _peakUsed) _peakUsed = _used;
        xSemaphoreGive(_mutex);
        return &_pool[idx];
    }

    void release(T* ptr) {
        if (!ptr) return;
        size_t idx = ptr - _pool;
        if (idx >= N) return;
        xSemaphoreTake(_mutex, portMAX_DELAY);
        _next[idx] = _free;
        _free = idx;
        _used--;
        xSemaphoreGive(_mutex);
    }

    size_t used()     const { return _used; }
    size_t capacity() const { return N; }
    size_t peak()     const { return _peakUsed; }
    bool   full()     const { return _free == NONE; }

private:
    static constexpr size_t NONE = SIZE_MAX;
    T                _pool[N];
    size_t           _next[N];
    size_t           _free     = NONE;
    size_t           _used     = 0;
    size_t           _peakUsed = 0;
    SemaphoreHandle_t _mutex   = nullptr;
};

// ─────────────────────────────────────────────────────────────
// StaticString<N> — fixed-capacity string, no heap.
// Replaces Arduino String in topic building and JSON paths.
// ─────────────────────────────────────────────────────────────
template<size_t N>
class StaticString {
public:
    StaticString()              { _buf[0] = '\0'; }
    explicit StaticString(const char* s) { assign(s); }

    void assign(const char* s)  { strlcpy(_buf, s ? s : "", N); }
    void append(const char* s)  { strlcat(_buf, s ? s : "", N); }

    template<typename... Args>
    void format(const char* fmt, Args... args) {
        snprintf(_buf, N, fmt, args...);
    }

    const char* c_str() const   { return _buf; }
    size_t      length() const  { return strlen(_buf); }
    bool        empty()  const  { return _buf[0] == '\0'; }
    void        clear()         { _buf[0] = '\0'; }

    bool operator==(const char* s) const { return strcmp(_buf, s) == 0; }

private:
    char _buf[N];
};

// ─────────────────────────────────────────────────────────────
// Global pool singletons — sized for observed peak usage
// ─────────────────────────────────────────────────────────────
#include "../core/Types.h"

// Forward-declared; defined in MemoryPool.cpp / firmware.ino
extern StaticPool<SensorReading, 32>  gSensorPool;
extern StaticPool<Alert,         8>   gAlertPool;

// Convenience RAII wrapper
template<typename Pool>
struct PoolGuard {
    using T = decltype(*((Pool*)nullptr)->acquire());
    Pool& pool;
    T*    ptr;
    explicit PoolGuard(Pool& p) : pool(p), ptr(p.acquire()) {}
    ~PoolGuard() { if (ptr) pool.release(ptr); }
    T* operator->() { return ptr; }
    T& operator*()  { return *ptr; }
    bool valid()    { return ptr != nullptr; }
};
