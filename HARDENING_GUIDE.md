# LumaRoK C10 — Hardening Guide (v3.0 → v3.1)

## What Was Hardened and Why

---

### 1. Memory Optimization (`core/MemoryPool.h`)

**Problem:** Arduino `String` heap allocations in sensor loops (100ms IR, 500ms door,
10s sensor) caused progressive heap fragmentation. After days of runtime,
`heap_caps_get_largest_free_block()` could drop below 50% of `getFreeHeap()`.

**Fix:**
- `StaticPool<T,N>` — fixed freelist pool for `SensorReading` (32 slots) and `Alert` (8 slots). Zero heap allocations after `init()`.
- `StaticString<N>` — stack-local fixed-capacity string replaces `String` in topic builders.
- `BatchPublisher` uses a `StaticJsonDocument<1536>` — no heap JSON serialization.
- All MQTT topic strings pre-built into `char[]` at boot (`MQTTTopics::init()`).

**Monitoring:** `HeapMonitor::fragRatio()` — published every 60s in diagnostics.
Target: > 0.6. Below 0.4 = actionable fragmentation.

---

### 2. Reconnect Resilience (`networking/ReconnectEngine.h`)

**Problem:**
- `WiFiManager` used fixed 10s polling with no jitter → reconnect storms on fleet restart.
- No detection of zombie connections (WiFi stack says connected, RSSI = 0, no traffic).
- MQTT reconnect only triggered by WiFi watchdog, not independently.
- No metrics to distinguish frequent disconnects from one-time events.

**Fix — `ReconnectEngine`:**
- Full jitter exponential backoff on both WiFi and MQTT independently (`RetryPolicy`).
  Formula: `sleep = rand(0, min(cap, base × 2^attempt))` — prevents thundering herd.
- Zombie detection: RSSI == 0 check + MQTT inactivity > 2× keepalive triggers force-reconnect.
- `CircuitBreaker` per layer: opens after 10 WiFi failures / 5 MQTT failures,
  prevents futile retry loops while allowing periodic probe (half-open state).
- Reconnect metrics (`wifiReconnects`, `mqttReconnects`, `zombieDetections`) persisted to NVS,
  published in diagnostics for fleet-level observability.

---

### 3. Crash Recovery (`core/BootManager.h`, `diagnostics/CrashLogger.h`)

**Problem:**
- Crash counter incremented but rollback never triggered.
- No crash context captured before reset.
- No distinction between OTA-triggered restart and panic reset.

**Fix:**
- `CrashLogger` registers `esp_register_shutdown_handler` — fires before any reset.
  Captures: reset reason, task name, last MQTT topic, uptime, free heap, crash count.
  Written to NVS namespace `lmr_diag` as a blob.
- On next boot, `DiagnosticsManager::publishCrashRecord()` reads and publishes the
  blob via MQTT (`/system/diagnostics`) then clears it.
- `BootManager::init()` only increments crash counter on `ESP_RST_PANIC`,
  `ESP_RST_TASK_WDT`, `ESP_RST_INT_WDT` — not on clean OTA or manual restarts.
- Stable operation (5 min connected) clears counter → no false rollback triggers.

---

### 4. Watchdog Recovery (`diagnostics/WatchdogManager.h`)

**Problem:**
- Single hardware WDT registered only for the loop task.
- Blocking operations (OTA, stepper, WiFi connect) starved the hardware WDT silently.
- No per-task liveness tracking — a hung task went undetected until hardware WDT fired.

**Fix — `WatchdogManager`:**
- Every task calls `registerTask(name, timeoutMs)` and `checkin(idx)` in its main loop.
- Soft scan every 1s from `DiagnosticsTask` — detects missed check-ins before the
  hardware WDT fires, logs the offending task name and age.
- Escalation: 3 consecutive missed check-ins → deliberately stops feeding the hardware
  WDT for that task → controlled panic with full crash log.
- OTA task calls `suspend(idx)` / `resume(idx)` to exempt itself during download;
  hardware WDT extended to 180s via `TaskManager::extendWatchdog()`.
- Stack high-water marks reported per task every 60s. Alert if < 512 bytes remaining.

---

### 5. OTA Rollback Safety (`ota/OTAManagerV2.h`, `ota/RollbackManager.h`)

**Problem:**
- `Update.canRollBack()` checked but rollback never invoked automatically.
- No attempt counter — a bad OTA binary that crashes on boot loops indefinitely.
- OTA shared TLS client with MQTT (fixed in v3.0, reinforced here).
- No partition sanity check after write, before reboot.

**Fix — `OTAManagerV2`:**
- Attempt counter stored in NVS (`ota_att`). Incremented before download; cleared on success.
- If `ota_att >= 3` at boot: `OTAManagerV2::init()` transitions to `ROLLBACK_CHECK` state
  and calls `Update.rollBack()` before any tasks start.
- `Update.end(true)` result checked; `Update.isFinished()` verified before applying.
- Flash flushed (`FlashWearGuard::flushAll()`) before OTA restart to prevent partial writes.
- Download headers include `X-Unit-Id` and `X-FW-Version` for backend-side audit logging.
- Progress published every 5% (was every 10%) with byte count for download rate monitoring.

---

### 6. Telemetry Batching (`telemetry/BatchPublisher.h`)

**Problem:**
- Each sensor reading triggered a separate `mqttClient.publish()`.
- 5 sensor types × every 10s + IR at 10Hz = up to ~15 MQTT publishes/min.
- Each publish: heap allocation for topic String + payload buffer.

**Fix — `BatchPublisher`:**
- Accumulates up to 8 `SensorReading` structs in a static array.
- Flushes to one MQTT message after 8s window or when batch is full.
- Reduces publish rate by ~8× (sensor telemetry: 5 publishes/min → <1).
- Static `StaticJsonDocument<1536>` — no heap JSON serialization.
- High-priority readings (alerts, state changes) bypass batching and flush immediately.
- Batch payload includes `seq` counter for duplicate detection at backend.

---

### 7. Flash Wear Reduction (`storage/FlashWearGuard.h`)

**Problem:**
- `Preferences::putBool()` called on every relay toggle (device state persistence).
- Ring buffer head/tail written on every sensor read (every 10s = 8,640 writes/day).
- Crash counter written on every unexpected reset.
- NVS flash endurance: ~10,000 erase cycles per sector. At 8,640 writes/day, sectors
  wear out in ~1.2 days under worst-case churn.

**Fix — `FlashWearGuard`:**
- Write coalescing: all NVS writes queued in a 24-entry RAM table.
- FreeRTOS software timer flushes the table 2s after last write (quiet period).
- If same key is written multiple times within the window, only the last value is written.
  Example: relay toggled 3 times in 2s → 1 NVS write instead of 3.
- `immediate=true` flag for safety-critical writes (crash counter, prov lock) — bypasses coalescing.
- Daily write budget tracking with warning at 500 writes/day.
- Lifetime write counter persisted every 100 writes (not every write).
- `flushAll()` called before OTA restart and before deep sleep.

---

### 8. Offline Buffering (`storage/PriorityBuffer.h`)

**Problem:**
- Single ring buffer (64 entries) treated all telemetry equally.
- A gas alert could be evicted by 64 consecutive temperature readings.
- NVS writes on every `push()` → massive flash wear during extended outages.

**Fix — `PriorityBuffer`:**
- Two-tier separation:
  - **HIGH** (16 slots RAM + 32 slots NVS overflow): alerts, door/gas events. Never dropped.
  - **NORMAL** (48 slots RAM): sensor telemetry. Oldest dropped on overflow.
- High-priority overflow spills to NVS only (not dropped), with a distinct key namespace.
- Normal-priority overflow drops oldest silently with `_droppedCount` counter published in diagnostics.
- `peek()` always returns HIGH before NORMAL — priority ordering guaranteed.

---

### 9. Retry Backoff (`core/RetryPolicy.h`)

**Problem:** Fixed retry intervals risked reconnect storms across a fleet of deployed units.

**Fix — `RetryPolicy` (full jitter):**
```
delay = random(0, min(cap, base × 2^attempt))
```
- Base: 2s, Cap: 120s (WiFi) / 60s (MQTT).
- `esp_random()` for cryptographic-quality jitter — no `rand()` seed issues.
- `CircuitBreaker` wraps each connection layer independently.
  CLOSED → OPEN after N failures → HALF_OPEN after timeout → CLOSED on success.

---

### 10. Diagnostics Logging (`tasks/DiagnosticsTaskV2.h`)

**New structured diagnostics payload (every 60s):**
```json
{
  "unit": "LMR-A4CF12345678",
  "fw": "3.1.0",
  "uptime": 86400,
  "heap": { "free": 187432, "min": 124016, "largest": 102400 },
  "flash_writes": 1204,
  "reconnects": { "wifi": 2, "mqtt": 1, "wifi_fail": 0, "mqtt_fail": 0, "zombies": 0 },
  "buffer": { "high": 0, "normal": 0, "nvsq": 0, "dropped": 0 },
  "batch": { "pubs": 8640, "readings": 69120 },
  "mode": 0,
  "tasks": {
    "SafetyTask":  { "missed": 0, "checkins": 86400, "stack_hwm": 2180, "ok": true },
    "SensorTask":  { "missed": 0, "checkins": 86400, "stack_hwm": 1640, "ok": true },
    "NetworkTask": { "missed": 0, "checkins": 86400, "stack_hwm": 2048, "ok": true }
  },
  "ts": 1748086400
}
```

---

### 11. Fault Tolerance (`diagnostics/FaultManager.h`)

**Problem:** A failed DHT sensor or I2C bus error crashed the device rather than degrading gracefully.

**Fix — `FaultManager` with `SystemMode` escalation:**

| Mode | Active when | Behavior |
|---|---|---|
| `NORMAL` | No faults | Full operation |
| `DEGRADED` | 2+ medium faults | Suspends occupancy/parking automation; core safety continues |
| `SAFE` | 1+ high fault | Safety + MQTT only; no telemetry batching, no OTA |
| `FAILED` | Critical fault | Logs, publishes, restarts after 2s |

- Per-fault circuit breaker recovery triggers (I2C bus recovery, stepper home).
- Faults auto-clear when subsystem reports success.
- Active fault summary published every 2 min if any faults present.

---

## Migration from v3.0 to v3.1

1. **Replace `firmware.ino`** with `firmware_hardened.ino`.
2. **Replace `tasks/NetworkTask.h`** references with `tasks/NetworkTaskV2.h`.
3. **Replace `tasks/SensorTask.h`** references with `tasks/SensorTaskV2.h`.
4. **Replace `tasks/OTAAndDiagTasks.h`** with `tasks/OTATaskV2.h` + `tasks/DiagnosticsTaskV2.h`.
5. **Replace `ota/OTAManager.h`** with `ota/OTAManagerV2.h`.
6. **Add new calls to `setup()`**: `WatchdogManager::init()`, `FaultManager::init()`, `FlashWearGuard::init()`, `PriorityBuffer::init()`.
7. **Sensor publish calls**: replace `TelemetryPipeline::publish(r)` with `BatchPublisher::stage(r)`.
8. **NVS writes in hot paths**: replace `NVSStore::putBool/putU8` with `FlashWearGuard::putBool/putU8`.
9. All existing MQTT topic contracts, device registry, and provisioning flows are **unchanged**.

## Build Environments

| Environment | Use case | Serial prov | Log level | BLE |
|---|---|---|---|---|
| `production` | Fleet OTA | OFF | ERROR | ON |
| `factory` | Production line | ON | INFO | ON |
| `development` | Dev/debug | ON | DEBUG | ON |
| `ota_update` | Minimal OTA image | OFF | ERROR | OFF |

```bash
pio run -e production     # production binary
pio run -e factory        # factory flash binary  
pio run -e development    # development binary with full logging
pio run -e ota_update     # smallest OTA binary (~80KB smaller, no BLE)
```


---

## v3.2 — Design Upgrades and Bug Fixes (over v3.1)

### Bug Fixes (all previously silent or compile-breaking)

| # | File | Severity | Description |
|---|---|---|---|
| 1 | `core/Config.h` | Minor | `FIRMWARE_VERSION` corrected from `"3.1.0"` to `"3.2.0"` |
| 2 | `diagnostics/FaultManager.h` | **Compile Error** | Added `TASK_CREATE_FAIL = 11` to `FaultCode` enum — `TaskManagerV2` referenced it but it did not exist |
| 3 | `ota/OTAVerifier.h` | **Critical Security** | URL host whitelist used suffix-match (`strncmp` on trailing bytes). An attacker-controlled subdomain of any whitelisted host would pass the check. Replaced with exact hostname comparison. |
| 4 | `tasks/MQTTTasks.h` | Medium | TX queue watermark threshold `> 50` was dead code — queue depth max is 16. Replaced with `>= MQTT_TX_QUEUE_DEPTH * 3/4` (12) for warn and `>= MQTT_TX_QUEUE_DEPTH` (16) for error. |
| 5 | `networking/ReconnectEngine.h` | Medium | MQTT zombie detector initialises `_lastMqttActivityMs = 0`. On first connect, if broker sends no PUBLISH within 120s (2× keepalive), zombie logic force-disconnects a healthy session. Fixed: reset timer to `millis()` on successful connect. |
| 6 | `mqtt/MQTTTransport.h` | Medium | `connect()` checks `isConnected()` under mutex, releases it, then re-acquires for `_client->connect()`. A second caller could connect in the gap; the first caller then redundantly reconnects. Fixed: re-check `_client->connected()` inside the second mutex window. |
| 7 | `core/EventBus.h` | Low | `post()` default branch routed all unhandled `EventType` values (HEARTBEAT_TICK, WIFI_CONNECTED, MQTT_CONN_OK, etc.) to `postSensor()`, wasting sensor queue slots and displacing telemetry during reconnect storms. All EventTypes now explicitly routed; unknown types are logged and dropped. |

### Design Upgrades (additive — no architecture removed)

| # | File | Description |
|---|---|---|
| A | `core/Config.h` | Added `HEAP_FREE_WARN_BYTES` (90 KB), `HEAP_FREE_CRIT_BYTES` (60 KB), `HEAP_FRAG_WARN_PCT` (60%), `HEAP_FRAG_CRIT_PCT` (40%) — soak-test-calibrated thresholds used by DiagnosticsTask |
| B | `core/Config.h` | Added compile-time `#error` guard: building with `US2_AVAILABLE != 0` on a non-S3 target (where US2 and DOOR_LOCK share GPIO4) is now a build-time error instead of a silent hardware conflict |
| C | `tasks/DiagnosticsTaskV2.h` | Heap thresholds (A above) wired into `FaultManager::record(HEAP_LOW)` on every diagnostics tick. Devices now self-report `HEAP_LOW` faults at the correct thresholds rather than requiring manual inspection of diagnostics JSON |

### Migration from v3.1 to v3.2

All changes are drop-in replacements of the modified files. No API changes, no new `setup()` calls, no new task registrations. Rebuild with `pio run -e production` and OTA-push the resulting binary.

**Verification checklist post-update:**
1. Confirm `FIRMWARE_VERSION` in diagnostics JSON reads `"3.2.0"`.
2. Confirm no build error on non-S3 targets (GPIO guard fires correctly on misconfigured builds).
3. Verify OTA rejects URLs like `https://evil-lumarok-backend.onrender.com/fw.bin` — should log `URL host not whitelisted`.
4. During soak: confirm `HEAP_LOW` fault appears in diagnostics when heap drops below 90 KB free.
5. Verify TX queue logs appear at 12 messages queued (not 50+).
