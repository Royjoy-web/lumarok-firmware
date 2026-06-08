#pragma once
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <freertos/queue.h>
#include "../core/Types.h"
#include "../core/Config.h"
#include "../core/EventBus.h"
#include "../core/Identity.h"
#include "../mqtt/MQTTTopics.h"
#include "../security/CredentialStore.h"
#include "../networking/ReconnectEngine.h"

// MQTTPub payload cap: largest real payload is the diagnostics JSON (~896 bytes).
// 1024 bytes gives comfortable headroom and fits well within MQTT_BUFFER_SIZE (4096).
#define MQTT_PUB_PAYLOAD_MAX 1024
#define MQTT_TX_QUEUE_DEPTH  16   // 16 × ~608 bytes ≈ 9.5 KB

struct MQTTPub {
    char    topic[96];
    char    payload[MQTT_PUB_PAYLOAD_MAX];
    uint8_t qos;
    bool    retain;
};

// ── Subscription Registry ─────────────────────────────────────
#define MQTT_MAX_SUBS 8
using MQTTCallback = void(*)(const char* topic, const uint8_t* payload, unsigned int len);

struct MQTTSub {
    char         topic[96];
    MQTTCallback callback;
};

// ── WiFiClient adapter shim ───────────────────────────────────
class _WiFiClientAdapter : public WiFiClient {
    WiFiClientSecure& _sc;
public:
    explicit _WiFiClientAdapter(WiFiClientSecure& sc) : _sc(sc) {}
    int     connect(const char* h, uint16_t p) override { return _sc.connect(h, p); }
    int     connect(IPAddress ip, uint16_t p)  override { return _sc.connect(ip, p); }
    size_t  write(const uint8_t* b, size_t s)  override { return _sc.write(b, s); }
    size_t  write(uint8_t b)                   override { return _sc.write(b); }
    int     available()                         override { return _sc.available(); }
    int     read()                              override { return _sc.read(); }
    int     read(uint8_t* b, size_t s)          override { return _sc.read(b, s); }
    int     peek()                              override { return _sc.peek(); }
    void    flush()                             override { _sc.flush(); }
    void    stop()                              override { _sc.stop(); }
    uint8_t connected()                         override { return _sc.connected(); }
    operator bool()                             override { return (bool)_sc; }
};

class MQTTTransport {
public:
    static void init() {
        // _clientMutex serialises ALL PubSubClient calls:
        //   MQTTRxTask  → loop()
        //   MQTTTxTask  → drainOne()
        //   NetworkTask → connect() / subscribe()
        // PubSubClient shares one socket buffer — NOT thread-safe natively.
        _clientMutex = xSemaphoreCreateMutex();
        configASSERT(_clientMutex);
        _secureClient.setCACert(MQTT_ROOT_CA);
        _adapter = new _WiFiClientAdapter(_secureClient);
        _client  = new PubSubClient(*_adapter);
        _client->setServer(MQTT_BROKER, MQTT_PORT);
        _client->setBufferSize(MQTT_BUFFER_SIZE);
        _client->setKeepAlive(MQTT_KEEPALIVE_S);
        _client->setSocketTimeout(10);
        _client->setCallback(_incomingCallback);

        _txQueue = xQueueCreate(MQTT_TX_QUEUE_DEPTH, sizeof(MQTTPub));
        configASSERT(_txQueue);
        LOG_I("MQTT", "Transport initialised");
    }

    // ── Connection (NetworkTask) ──────────────────────────────
    static bool connect() {
        {
            if (!_takeMutex(100)) return false;
            bool already = _client->connected();
            _giveMutex();
            if (already) return true;
        }
        if (!_backoffExpired()) return false;

        String user = CredentialStore::mqttUser();
        String pass = CredentialStore::mqttPass();
        String cid  = Identity::mqttClientId();
        LOG_I("MQTT", "Connecting as %s", cid.c_str());

        if (!_takeMutex(5000)) return false;
        // FIX v3.2: re-check connected() inside the mutex before calling connect().
        // Between the first isConnected() check (above) and this lock acquisition,
        // another code path (e.g. a second ReconnectEngine tick) may have already
        // established the connection. Calling _client->connect() on an already-
        // connected session tears it down and reconnects unnecessarily.
        if (_client->connected()) {
            _giveMutex();
            return true;
        }
        bool ok = _client->connect(
            cid.c_str(), user.c_str(), pass.c_str(),
            MQTTTopics::lwtTopic(), 1, true, MQTTTopics::lwtPayload()
        );
        // FIX v3.2b (BUG-2): Both calls MUST execute while _clientMutex is still held.
        // The original code released the mutex first, then called these — racing with
        // MQTTRxTask::loop() which re-acquires the mutex every 50 ms on Core 0.
        if (ok) {
            _resubscribeAll();   // under mutex — safe
            _publishOnline();    // under mutex — safe
        }
        _giveMutex();

        _backoffStartMs = millis();

        if (ok) {
            _reconnectCount++;
            _backoffRetry = 0;
            EventBus::setNetBit(NET_MQTT_CONNECTED_BIT);
            // OPP-2: MQTT_CONN_OK postSensor removed — EventGroup bit above is the only consumer.
            LOG_I("MQTT", "Connected (reconnects: %d)", (int)_reconnectCount);
        } else {
            _backoffRetry++;
            LOG_W("MQTT", "Failed (rc=%d, retry=%d)", _client->state(), (int)_backoffRetry);
        }
        return ok;
    }

    // MQTTRxTask — pumps inbound messages + keepalive
    static void loop() {
        if (!_takeMutex(50)) return;
        if (_client->connected()) _client->loop();
        _giveMutex();
        // NOTE: _incomingCallback is invoked from within _client->loop()
        // while _clientMutex is held. Callbacks MUST NOT call any
        // MQTTTransport method that acquires _clientMutex — that would
        // deadlock. The registered callbacks (CommandDispatcher) only
        // post to EventBus queues, which is safe.
    }

    // ── Publish API (any task) ────────────────────────────────
    static bool publish(const char* topic, const char* payload,
                        uint8_t qos = 0, bool retain = false) {
        if (!topic || !payload) return false;
        MQTTPub pub{};
        strlcpy(pub.topic,   topic,   sizeof(pub.topic));
        strlcpy(pub.payload, payload, sizeof(pub.payload));
        if (strlen(payload) >= sizeof(pub.payload))
            LOG_W("MQTT", "Payload truncated to %u bytes: %s",
                  (unsigned)sizeof(pub.payload), topic);
        pub.qos    = qos;
        pub.retain = retain;
        if (xQueueSend(_txQueue, &pub, pdMS_TO_TICKS(10)) != pdTRUE) {
            LOG_W("MQTT", "TX queue full — dropping: %s", topic);
            return false;
        }
        return true;
    }

    // Direct publish — MQTTTxTask only, mutex-protected
    static bool drainOne() {
        MQTTPub pub{};
        if (xQueueReceive(_txQueue, &pub, 0) != pdTRUE) return false;
        if (!_takeMutex(200)) { xQueueSendToFront(_txQueue, &pub, 0); return false; }
        bool connected = _client->connected();
        bool ok = connected && _client->publish(pub.topic, pub.payload, pub.retain);
        _giveMutex();
        if (ok)            ReconnectEngine::notifyMQTTActivity();
        else if (!connected) xQueueSendToFront(_txQueue, &pub, 0);
        else               LOG_W("MQTT", "Publish failed: %s", pub.topic);
        return ok;
    }

    // ── Subscription Registry ─────────────────────────────────
    // FIX v3.2: _subs[] is now written inside _clientMutex.
    //
    // RACE IN v3.1:
    //   subscribe() wrote to _subs[_subCount] and incremented _subCount
    //   WITHOUT holding _clientMutex. Meanwhile _incomingCallback() reads
    //   _subs[] and _subCount from within loop() which DOES hold the mutex.
    //   NetworkTask (Core 0) and MQTTRxTask (Core 0) share the core so
    //   preemption between them is possible — the race was real.
    //
    // FIX: Write _subs[] under the mutex. The client->subscribe() call is
    //   now also made in the same lock window, saving one extra acquisition.
    //   Since callbacks only call EventBus (not MQTTTransport), no deadlock
    //   can result from the callback running while the mutex is held.
    static bool subscribe(const char* topic, MQTTCallback cb, uint8_t qos = 1) {
        if (!_takeMutex(500)) return false;
        bool ok = false;
        if (_subCount < MQTT_MAX_SUBS) {
            strlcpy(_subs[_subCount].topic, topic, sizeof(_subs[0].topic));
            _subs[_subCount].callback = cb;
            _subCount++;
            if (_client->connected()) _client->subscribe(topic, qos);
            ok = true;
        } else {
            LOG_E("MQTT", "Subscription table full — cannot subscribe: %s", topic);
        }
        _giveMutex();
        return ok;
    }

    // ── State ─────────────────────────────────────────────────
    static bool isConnected() {
        if (!_takeMutex(50)) return false;
        bool c = _client && _client->connected();
        _giveMutex();
        return c;
    }
    static int state() {
        if (!_takeMutex(50)) return -99;
        int s = _client ? _client->state() : -99;
        _giveMutex();
        return s;
    }
    static uint32_t  reconnectCount()  { return _reconnectCount; }
    static UBaseType_t txQueueDepth()  { return uxQueueMessagesWaiting(_txQueue); }

    static void disconnect() {
        if (!_takeMutex(500)) return;
        if (_client && _client->connected()) _client->disconnect();
        _giveMutex();
        EventBus::clearNetBit(NET_MQTT_CONNECTED_BIT);
    }

private:
    // Called from connect() which already holds _clientMutex
    static void _resubscribeAll() {
        for (uint8_t i = 0; i < _subCount; i++) {
            _client->subscribe(_subs[i].topic, 1);
            LOG_D("MQTT", "Re-subscribed: %s", _subs[i].topic);
        }
    }

    // Called from connect() while _clientMutex is held (BUG-2 fix).
    // Publishes to onlineTopic() (NOT lwtTopic()) — using lwtTopic() here
    // would overwrite the broker's retained LWT offline payload, causing
    // freshly-subscribing clients to receive the online JSON as the LWT.
    static void _publishOnline() {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "{\"online\":true,\"fw\":\"%s\",\"unit\":\"%s\"}",
                 FIRMWARE_VERSION, Identity::get().c_str());
        _client->publish(MQTTTopics::onlineTopic(), buf, true);
    }

    static bool _takeMutex(TickType_t msWait = portMAX_DELAY) {
        return xSemaphoreTake(_clientMutex, pdMS_TO_TICKS(msWait)) == pdTRUE;
    }
    static void _giveMutex() { xSemaphoreGive(_clientMutex); }

    // Invoked from _client->loop() while _clientMutex is held.
    // MUST NOT call any MQTTTransport method that acquires _clientMutex.
    static void _incomingCallback(const char* topic,
                                   uint8_t* payload, unsigned int len) {
        ReconnectEngine::notifyMQTTActivity();
        for (uint8_t i = 0; i < _subCount; i++) {
            if (_topicMatches(_subs[i].topic, topic)) {
                _subs[i].callback(topic, payload, len);
                return;
            }
        }
        LOG_W("MQTT", "Unrouted topic: %s", topic);
    }

    static bool _topicMatches(const char* filter, const char* topic) {
        while (*filter && *topic) {
            if (*filter == '#') return true;
            if (*filter == '+') {
                while (*topic && *topic != '/') topic++;
                filter++;
            } else {
                if (*filter != *topic) return false;
                filter++; topic++;
            }
        }
        return (*filter == '\0' && *topic == '\0') || (*filter == '#');
    }

    static bool _backoffExpired() {
        if (_backoffStartMs == 0) return true;
        uint32_t base = min((uint32_t)(MQTT_RECONNECT_BASE_MS << min(_backoffRetry, (uint8_t)6)),
                             (uint32_t)MQTT_RECONNECT_MAX_MS);
        return (millis() - _backoffStartMs) >= base;
    }

    static WiFiClientSecure    _secureClient;
    static _WiFiClientAdapter* _adapter;
    static PubSubClient*       _client;
    static SemaphoreHandle_t   _clientMutex;
    static QueueHandle_t       _txQueue;
    static MQTTSub             _subs[MQTT_MAX_SUBS];
    static uint8_t             _subCount;
    static uint32_t            _reconnectCount;
    static uint8_t             _backoffRetry;
    static unsigned long       _backoffStartMs;
};

inline WiFiClientSecure    MQTTTransport::_secureClient;
inline _WiFiClientAdapter* MQTTTransport::_adapter       = nullptr;
inline PubSubClient*       MQTTTransport::_client        = nullptr;
inline SemaphoreHandle_t   MQTTTransport::_clientMutex   = nullptr;
inline QueueHandle_t       MQTTTransport::_txQueue       = nullptr;
inline MQTTSub             MQTTTransport::_subs[MQTT_MAX_SUBS] = {};
inline uint8_t             MQTTTransport::_subCount      = 0;
inline uint32_t            MQTTTransport::_reconnectCount = 0;
inline uint8_t             MQTTTransport::_backoffRetry  = 0;
inline unsigned long       MQTTTransport::_backoffStartMs = 0;
