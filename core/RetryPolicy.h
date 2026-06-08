#pragma once
#include <Arduino.h>

// ─────────────────────────────────────────────────────────────
// RetryPolicy — full jitter exponential backoff.
// Algorithm: sleep = rand(0, min(cap, base * 2^attempt))
// "Full jitter" avoids thundering herd on reconnect storms.
// ─────────────────────────────────────────────────────────────
class RetryPolicy {
public:
    struct Config {
        uint32_t baseMs;
        uint32_t capMs;
        uint8_t  maxTries;
        Config(uint32_t base=1000, uint32_t cap=120000, uint8_t tries=0)
            : baseMs(base), capMs(cap), maxTries(tries) {}
    };

    explicit RetryPolicy(Config cfg = Config()) : _cfg(cfg) { reset(); }

    // Call after a failure. Returns delay to wait before next attempt.
    uint32_t nextDelayMs() {
        uint32_t slot = min(_cfg.capMs,
                            _cfg.baseMs * (1u << min(_attempt, (uint32_t)10)));
        uint32_t delay = (uint32_t)(esp_random() % (slot + 1));
        _attempt++;
        _lastAttemptMs = millis();
        return delay;
    }

    // Returns true if enough time has passed since last attempt
    bool ready() const {
        if (_attempt == 0) return true;
        return (millis() - _lastAttemptMs) >= _pendingDelayMs;
    }

    // Call on success
    void reset() {
        _attempt       = 0;
        _lastAttemptMs = 0;
        _pendingDelayMs= 0;
    }

    // Schedule next attempt with computed delay
    void scheduleNext() {
        _pendingDelayMs = nextDelayMs();
    }

    bool limitReached() const {
        return _cfg.maxTries > 0 && _attempt >= _cfg.maxTries;
    }

    uint32_t attempt()      const { return _attempt; }
    uint32_t pendingDelayMs()const{ return _pendingDelayMs; }

private:
    Config       _cfg;
    uint32_t     _attempt        = 0;
    unsigned long _lastAttemptMs = 0;
    uint32_t     _pendingDelayMs = 0;
};

// ─────────────────────────────────────────────────────────────
// CircuitBreaker — CLOSED → OPEN → HALF_OPEN → CLOSED.
// Prevents cascading failures (e.g. MQTT, HTTP, I2C).
// ─────────────────────────────────────────────────────────────
enum class CBState : uint8_t { CLOSED, OPEN, HALF_OPEN };

class CircuitBreaker {
public:
    struct Config {
        uint8_t  failThreshold;
        uint8_t  successToClose;
        uint32_t openDurationMs;
        Config(uint8_t fail=5, uint8_t succ=2, uint32_t openMs=30000)
            : failThreshold(fail), successToClose(succ), openDurationMs(openMs) {}
    };

    explicit CircuitBreaker(const char* name, Config cfg = Config())
        : _name(name), _cfg(cfg) {}

    // Call before attempting an operation
    bool allowRequest() {
        switch (_state) {
            case CBState::CLOSED:    return true;
            case CBState::HALF_OPEN: return true;
            case CBState::OPEN:
                if (millis() - _openedAtMs >= _cfg.openDurationMs) {
                    _state = CBState::HALF_OPEN;
                    LOG_I("CB", "[%s] HALF_OPEN — probing", _name);
                    return true;
                }
                return false;
        }
        return false;
    }

    // Call after a successful operation
    void recordSuccess() {
        if (_state == CBState::HALF_OPEN) {
            _successCount++;
            if (_successCount >= _cfg.successToClose) {
                _state        = CBState::CLOSED;
                _failCount    = 0;
                _successCount = 0;
                LOG_I("CB", "[%s] CLOSED", _name);
            }
        } else {
            _failCount = 0;
        }
    }

    // Call after a failed operation
    void recordFailure() {
        _failCount++;
        _successCount = 0;
        if (_state == CBState::CLOSED && _failCount >= _cfg.failThreshold) {
            _state      = CBState::OPEN;
            _openedAtMs = millis();
            LOG_W("CB", "[%s] OPEN after %d failures", _name, (int)_failCount);
        } else if (_state == CBState::HALF_OPEN) {
            _state      = CBState::OPEN;
            _openedAtMs = millis();
            LOG_W("CB", "[%s] OPEN (half-open probe failed)", _name);
        }
    }

    CBState     state()      const { return _state; }
    bool        isOpen()     const { return _state == CBState::OPEN; }
    bool        isClosed()   const { return _state == CBState::CLOSED; }
    uint8_t     failCount()  const { return _failCount; }
    const char* name()       const { return _name; }

private:
    const char*   _name;
    Config        _cfg;
    CBState       _state        = CBState::CLOSED;
    uint8_t       _failCount    = 0;
    uint8_t       _successCount = 0;
    unsigned long _openedAtMs   = 0;
};
