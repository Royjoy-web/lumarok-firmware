#pragma once
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include "../core/Config.h"
#include "../core/Types.h"

// StepperHAL — non-blocking 28BYJ-48 stepper via FreeRTOS software timer.
// Original firmware had a blocking while-loop that stalled the main thread
// for ~1 second. This implementation advances one step per 2ms timer tick
// and fires a completion callback when the target is reached.
class StepperHAL {
public:
    using CompletionCb = void(*)(bool reached_target);

    static void init() {
        pinMode(PIN_STEPPER_IN1, OUTPUT);
        pinMode(PIN_STEPPER_IN2, OUTPUT);
        pinMode(PIN_STEPPER_IN3, OUTPUT);
        pinMode(PIN_STEPPER_IN4, OUTPUT);
        _deenergize();

        _timer = xTimerCreate("Stepper", pdMS_TO_TICKS(2),
                               pdTRUE, nullptr, _timerCb);
        configASSERT(_timer);
    }

    // Move to absolute step position (0 = closed, STEPPER_STEPS = fully open)
    static void moveTo(int targetPos, CompletionCb cb = nullptr) {
        if (_moving) return;   // ignore if already moving
        _targetPos = constrain(targetPos, 0, STEPPER_STEPS);
        _completionCb = cb;
        if (_currentPos == _targetPos) {
            if (cb) cb(true);
            return;
        }
        _moving = true;
        xTimerStart(_timer, 0);
    }

    static void stop() {
        xTimerStop(_timer, 0);
        _deenergize();
        _moving = false;
    }

    static bool     isMoving()    { return _moving; }
    static int      currentPos()  { return _currentPos; }
    static int      targetPos()   { return _targetPos; }

    // Obstacle detected by US3 — halt and report
    static void emergencyStop() {
        stop();
        LOG_W("Stepper", "Emergency stop at step %d", _currentPos);
        if (_completionCb) _completionCb(false);
    }

private:
    // Half-step sequence for 28BYJ-48
    static constexpr int HALF_STEPS[8][4] = {
        {1,0,0,0}, {1,1,0,0}, {0,1,0,0}, {0,1,1,0},
        {0,0,1,0}, {0,0,1,1}, {0,0,0,1}, {1,0,0,1}
    };

    static void _timerCb(TimerHandle_t) {
        if (!_moving) return;

        // Advance one step toward target
        int dir = (_targetPos > _currentPos) ? 1 : -1;
        _stepIndex = (_stepIndex + dir + 8) % 8;
        _applyStep(_stepIndex);
        _currentPos += dir;

        if (_currentPos == _targetPos) {
            xTimerStopFromISR(_timer, nullptr);
            _deenergize();
            _moving = false;
            if (_completionCb) _completionCb(true);
        }
    }

    static void _applyStep(int idx) {
        const uint8_t pins[4] = {PIN_STEPPER_IN1, PIN_STEPPER_IN2,
                                  PIN_STEPPER_IN3, PIN_STEPPER_IN4};
        for (int i = 0; i < 4; i++)
            digitalWrite(pins[i], HALF_STEPS[idx][i]);
    }

    static void _deenergize() {
        digitalWrite(PIN_STEPPER_IN1, LOW);
        digitalWrite(PIN_STEPPER_IN2, LOW);
        digitalWrite(PIN_STEPPER_IN3, LOW);
        digitalWrite(PIN_STEPPER_IN4, LOW);
    }

    static TimerHandle_t _timer;
    static int           _currentPos;
    static int           _targetPos;
    static int           _stepIndex;
    static bool          _moving;
    static CompletionCb  _completionCb;
};

inline TimerHandle_t     StepperHAL::_timer        = nullptr;
inline int               StepperHAL::_currentPos   = 0;
inline int               StepperHAL::_targetPos    = 0;
inline int               StepperHAL::_stepIndex    = 0;
inline bool              StepperHAL::_moving       = false;
inline StepperHAL::CompletionCb StepperHAL::_completionCb = nullptr;

constexpr int StepperHAL::HALF_STEPS[8][4];
