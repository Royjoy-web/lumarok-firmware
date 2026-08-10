#pragma once
// IRTransmitter.h — IR TX via IRremoteESP8266
// Library: IRremoteESP8266 (install via Arduino Library Manager)
// Wire: IR LED + 2N2222 transistor on PIN_IR_TX (default GPIO4 on non-HVAC boards)
// Enable: #define ENABLE_IR_TX true  OR  #define ENABLE_HVAC true
#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <ir_Daikin.h>
#include <ir_Mitsubishi.h>
#include <ir_Gree.h>
#include "../core/Config.h"
#include "../core/EventBus.h"

#ifndef PIN_IR_TX
#define PIN_IR_TX 4   // override in Config.h if needed
#endif

class IRTransmitter {
public:
    static void init() {
        _ir = new IRsend(PIN_IR_TX);
        _ir->begin();
        LOG_I("IR", "IRTransmitter ready on GPIO%d", PIN_IR_TX);
    }

    // Raw NEC code (AC remotes, TV, etc.)
    static void sendNEC(uint64_t code, uint16_t bits = 32) {
        _ir->sendNEC(code, bits);
        LOG_D("IR", "NEC sent: 0x%llX", code);
    }

    // Raw code from stored profile (hex string from MQTT command)
    static void sendRaw(const uint16_t* buf, uint16_t len, uint16_t hz = 38) {
        _ir->sendRaw(buf, len, hz);
    }

    // Generic AC: protocol + mode + temp + fan
    struct ACCommand {
        char protocol[16]; // "DAIKIN", "MITSUBISHI_AC", "GREE"
        uint8_t  temp;     // 16–30°C
        uint8_t  mode;     // 0=cool 1=heat 2=dry 3=fan 4=auto
        uint8_t  fan;      // 0=auto 1–5=speed
        bool     power;
        bool     swing;
    };

    static void sendAC(const ACCommand& c) {
        if (strcmp(c.protocol, "DAIKIN") == 0) {
            IRDaikinESP ac(PIN_IR_TX);
            ac.begin();
            ac.setPower(c.power);
            ac.setTemp(c.temp);
            ac.setMode(c.mode);
            ac.setFan(c.fan);
            ac.setSwingVertical(c.swing);
            ac.send();
        } else if (strcmp(c.protocol, "MITSUBISHI_AC") == 0) {
            IRMitsubishiAC ac(PIN_IR_TX);
            ac.begin();
            ac.setPower(c.power);
            ac.setTemp(c.temp);
            ac.setMode(c.mode);
            ac.setFan(c.fan);
            ac.send();
        } else if (strcmp(c.protocol, "GREE") == 0) {
            IRGreeAC ac(PIN_IR_TX);
            ac.begin();
            ac.setPower(c.power);
            ac.setTemp(c.temp);
            ac.setMode(c.mode);
            ac.setFan(c.fan);
            ac.setSwingVertical(c.swing, kGreeSwingLastPos);
            ac.send();
        } else {
            LOG_W("IR", "Unknown AC protocol: %s", c.protocol);
            return;
        }
        {
            Event e{}; e.type = EventType::SAFETY_ALERT;
            strlcpy(e.data.alert.type, "ir_sent", sizeof(e.data.alert.type));
            snprintf(e.data.alert.message, sizeof(e.data.alert.message),
                     "%s temp=%d", c.protocol, c.temp);
            e.data.alert.device[0] = '\0';
            e.data.alert.severity  = AlertSeverity::INFO;
            e.data.alert.timestamp = 0;
            EventBus::postAlert(e);
        }
        LOG_I("IR", "AC sent: %s pwr=%d temp=%d mode=%d fan=%d", c.protocol, c.power, c.temp, c.mode, c.fan);
    }

private:
    static IRsend* _ir;
};
IRsend* IRTransmitter::_ir = nullptr;
