#pragma once
#include <Arduino.h>
#include <esp_sntp.h>
#include <time.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"

class TimeSync {
public:
    static void init() {
        configTzTime("UTC", "pool.ntp.org", "time.google.com");
        sntp_set_time_sync_notification_cb(_syncCb);
        LOG_I("NTP", "SNTP configured");
    }

    // Returns Unix epoch, or 0 if not yet synced
    static time_t now() {
        time_t t; time(&t);
        return (t > 1700000000UL) ? t : 0;
    }

    static bool isSynced() {
        return EventBus::isTimeSynced();
    }

    // Fallback timestamp — millis-based offset from last sync
    static time_t bestEffort() {
        time_t n = now();
        if (n) return n;
        // If we have a sync base, project forward via millis delta
        if (_syncBase && _syncMillis) {
            return _syncBase + (time_t)((millis() - _syncMillis) / 1000UL);
        }
        return 0;
    }

private:
    static void _syncCb(struct timeval* tv) {
        _syncBase   = (time_t)tv->tv_sec;
        _syncMillis = millis();
        EventBus::setNetBit(NET_TIME_SYNCED_BIT);
        // OPP-3: postSensor(TIME_SYNCED) removed — EventGroup bit is the consumer.

        LOG_I("NTP", "Time synced: epoch %lu", (unsigned long)_syncBase);
    }

    static time_t        _syncBase;
    static unsigned long _syncMillis;
};

inline time_t        TimeSync::_syncBase   = 0;
inline unsigned long TimeSync::_syncMillis = 0;
