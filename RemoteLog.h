/**
 * RemoteLog.h — Knob Controller remote logging for FirmwareManager
 * (MQTT over WebSocket).
 *
 * Based on the FirmwareManager ESP32 example client (RemoteLogExample).
 * Additions for this sketch:
 *   - init(): call at the very top of setup() so lines logged before Wi-Fi is
 *     up (boot, Wi-Fi portal, ...) are buffered and sent once MQTT connects.
 *   - flush(): push buffered lines out before ESP.restart() (OTA, /api/restart).
 *   - RLOG_SERIAL_PREFIX: optional local-only Serial prefix (default none).
 *
 * Connects to the broker built into the FirmwareManager backend and:
 *   - ships log lines in batches to   fm/<mac>/log     (QoS 1)
 *   - reports fw + levels on          fm/<mac>/status  (retained; LWT = offline)
 *   - applies log-level commands from fm/<mac>/cmd     (retained, from the console)
 *
 * <mac> is the Wi-Fi STA MAC as 12 lowercase hex digits; client id "fm-<mac>".
 * The protocol is model-agnostic: the device must be registered in the
 * console (Devices) with that MAC, and the server labels stored lines with
 * the registered model number ("###Logged msg: [<MODEL>] [I][TAG] text").
 *
 * Transport: ESP-IDF's built-in MQTT client (esp-mqtt) — supports wss://,
 * reconnects on its own and runs in its own task. No extra library needed.
 *
 *   Render / HTTPS host : "wss://<your-service>.onrender.com/mqtt"
 *   Local dev           : "ws://<pc-ip>:8080/mqtt"
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <mqtt_client.h>

// Declared directly: WiFiClientSecure ships its own "esp_crt_bundle.h" that
// can shadow ESP-IDF's. This symbol is the IDF one, with the full Mozilla CA
// bundle compiled into the core (verifies Render's certificate without a PEM).
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

// --- Broker protocol (fixed by the FirmwareManager server, same for all models)
#define RLOG_TOPIC_ROOT    "fm"   // topics: fm/<mac>/{log,status,cmd}
#define RLOG_CLIENT_PREFIX "fm-"  // MQTT client id: fm-<mac>

// Optional local-only Serial prefix, e.g. "[KNOB]". Never sent to the server.
#ifndef RLOG_SERIAL_PREFIX
#define RLOG_SERIAL_PREFIX ""
#endif

namespace RemoteLog {

enum Level : uint8_t { OFF = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

// ----------------------------------------------------------------------------
// Tunables
// ----------------------------------------------------------------------------
static constexpr size_t kMaxLine = 256;          // server accepts up to 1024
static constexpr size_t kBatchBytes = 1024;      // one MQTT publish per batch
static constexpr uint32_t kFlushMs = 2000;       // max delay before a batch is sent
static constexpr Level kDefaultRemote = INFO;
static constexpr Level kDefaultSerial = INFO;

// ----------------------------------------------------------------------------
// State (one instance program-wide; C++11-safe for core 2.x)
// ----------------------------------------------------------------------------
struct State {
  esp_mqtt_client_handle_t client = nullptr;
  volatile bool connected = false;
  volatile bool paused = false;
  volatile bool force = false;    // flush(): send now, ignore batching delay
  volatile bool sending = false;  // a batch was taken and is being published
  volatile Level remote = kDefaultRemote;
  volatile Level serial = kDefaultSerial;
  SemaphoreHandle_t lock = nullptr;
  TaskHandle_t task = nullptr;
  char buf[kBatchBytes + 1];
  size_t len = 0;
  uint32_t firstAt = 0;  // millis() of the oldest buffered line
  uint32_t dropped = 0;  // lines discarded while offline / overflowing
  char id[13];
  char clientId[48];
  char tLog[64], tStatus[64], tCmd[64];
  char fw[33];
};
inline State &S() {
  static State s;
  return s;
}

inline const char *levelName(Level l) {
  switch (l) {
    case OFF: return "off";
    case ERROR: return "error";
    case WARN: return "warn";
    case DEBUG: return "debug";
    default: return "info";
  }
}
inline char levelLetter(Level l) {
  switch (l) {
    case ERROR: return 'E';
    case WARN: return 'W';
    case DEBUG: return 'D';
    default: return 'I';
  }
}
inline Level clampLevel(uint8_t v) { return static_cast<Level>(v > DEBUG ? static_cast<uint8_t>(INFO) : v); }

// ----------------------------------------------------------------------------
// Internals
// ----------------------------------------------------------------------------
namespace detail {

/** Find "key":"<level>" in a tiny flat JSON object. */
inline bool jsonLevel(const char *json, const char *key, Level &out) {
  char pat[16];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  p = strchr(p, '"');
  if (!p) return false;
  ++p;
  for (uint8_t i = 0; i <= DEBUG; ++i) {
    const char *name = levelName(static_cast<Level>(i));
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0 && p[n] == '"') {
      out = static_cast<Level>(i);
      return true;
    }
  }
  return false;
}

/** Retained status: the server shows these as the device's applied levels. */
inline void publishStatus() {
  if (!S().client) return;
  char json[128];
  int n = snprintf(json, sizeof json, "{\"state\":\"online\",\"fw\":\"%s\",\"remote\":\"%s\",\"serial\":\"%s\"}", S().fw,
                   levelName(S().remote), levelName(S().serial));
  // enqueue = non-blocking, safe from any task including the MQTT event handler.
  esp_mqtt_client_enqueue(S().client, S().tStatus, json, n, 1, 1, true);
}

inline void saveLevels() {
  Preferences p;
  if (p.begin("rlog", false)) {
    p.putUChar("remote", S().remote);
    p.putUChar("serial", S().serial);
    p.end();
  }
}

inline void loadLevels() {
  Preferences p;
  // Read-write so the namespace is created on first boot (read-only open of a
  // missing namespace fails and logs an NVS error).
  if (p.begin("rlog", false)) {
    S().remote = clampLevel(p.getUChar("remote", kDefaultRemote));
    S().serial = clampLevel(p.getUChar("serial", kDefaultSerial));
    p.end();
  }
}

/** {"remote":"debug","serial":"warn"} — keys optional. Empty = cleared, ignore. */
inline void applyCommand(const char *json) {
  Level r = S().remote, s = S().serial;
  bool any = jsonLevel(json, "remote", r) | jsonLevel(json, "serial", s);
  if (!any) return;
  bool changed = (r != S().remote) || (s != S().serial);
  S().remote = r;
  S().serial = s;
  if (changed) saveLevels();
  publishStatus();  // always confirm, so the console shows "synced"
}

inline void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      S().connected = true;
#if ESP_IDF_VERSION_MAJOR >= 5
      esp_mqtt_client_subscribe_single(S().client, S().tCmd, 1);
#else
      esp_mqtt_client_subscribe(S().client, S().tCmd, 1);
#endif
      publishStatus();
      if (S().task) xTaskNotifyGive(S().task);  // send whatever piled up
      break;
    case MQTT_EVENT_DISCONNECTED:
      S().connected = false;
      break;
    case MQTT_EVENT_DATA: {
      // Commands are tiny (server caps them at 96 bytes) and never fragmented.
      size_t tl = strlen(S().tCmd);
      if (e->topic_len == static_cast<int>(tl) && strncmp(e->topic, S().tCmd, tl) == 0 && e->current_data_offset == 0 &&
          e->data_len == e->total_data_len && e->data_len > 0 && e->data_len < 128) {
        char cmd[128];
        memcpy(cmd, e->data, e->data_len);
        cmd[e->data_len] = '\0';
        applyCommand(cmd);
      }
      break;
    }
    default:
      break;
  }
}

/** Sends the buffered batch. Never holds S().lock while calling into esp-mqtt. */
inline void flushTask(void *) {
  static char out[kBatchBytes + 1];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    if (!S().connected || S().paused) continue;

    size_t n = 0;
    uint32_t dropped = 0;
    xSemaphoreTake(S().lock, portMAX_DELAY);
    bool due = S().len > 0 &&
               (S().force || S().len >= kBatchBytes * 3 / 4 || millis() - S().firstAt >= kFlushMs);
    if (due) {
      n = S().len;
      memcpy(out, S().buf, n);
      S().len = 0;
      dropped = S().dropped;
      S().dropped = 0;
      S().sending = true;
    }
    xSemaphoreGive(S().lock);
    if (n == 0) continue;

    if (out[n - 1] == '\n') --n;  // lines are joined by '\n'
    // publish() sends now (enqueue only goes out on the MQTT task's ~1 s poll);
    // QoS 1 keeps it in the outbox for redelivery until the server acks it.
    if (esp_mqtt_client_publish(S().client, S().tLog, out, n, 1, 0) < 0)
      esp_mqtt_client_enqueue(S().client, S().tLog, out, n, 1, 0, true);
    if (dropped) {
      char note[64];
      int m = snprintf(note, sizeof note, "[W][RLOG] %u log lines dropped (offline or buffer full)", (unsigned)dropped);
      esp_mqtt_client_publish(S().client, S().tLog, note, m, 1, 0);
    }
    S().sending = false;
  }
}

/** Append one line ("[I][TAG] text\n"); oldest lines go first if it's full. */
inline void enqueueLine(const char *line, size_t len) {
  if (!S().lock || len == 0) return;
  xSemaphoreTake(S().lock, portMAX_DELAY);
  while (S().len > 0 && S().len + len + 1 > kBatchBytes) {
    char *nl = static_cast<char *>(memchr(S().buf, '\n', S().len));
    size_t cut = nl ? static_cast<size_t>(nl - S().buf) + 1 : S().len;
    memmove(S().buf, S().buf + cut, S().len - cut);
    S().len -= cut;
    ++S().dropped;
  }
  if (S().len == 0) S().firstAt = millis();
  memcpy(S().buf + S().len, line, len);
  S().len += len;
  S().buf[S().len++] = '\n';
  bool full = S().len >= kBatchBytes * 3 / 4;
  xSemaphoreGive(S().lock);
  if (full && S().task) xTaskNotifyGive(S().task);
}

}  // namespace detail

// ----------------------------------------------------------------------------
// Public API
// ----------------------------------------------------------------------------

/**
 * Load saved levels and start buffering. Call first thing in setup() (after
 * Serial.begin). Safe to skip — begin() calls it — but then lines logged
 * before begin() only go to Serial.
 */
inline void init() {
  if (S().lock) return;
  detail::loadLevels();
  S().lock = xSemaphoreCreateMutex();
}

/**
 * Start the MQTT client. Call once Wi-Fi is connected (calling again is a
 * no-op). The client reconnects by itself after Wi-Fi or server drops.
 *
 * rootCaPem: optional PEM to pin a specific CA; nullptr = built-in CA bundle
 * (recommended for wss:// on Render). Ignored for ws:// and mqtt://.
 */
inline bool begin(const char *uri, const char *username, const char *password, const char *fwVersion,
                  const char *rootCaPem = nullptr) {
  if (S().client) return true;
  init();
  if (!S().lock) return false;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(S().id, sizeof S().id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(S().clientId, sizeof S().clientId, RLOG_CLIENT_PREFIX "%s", S().id);
  snprintf(S().tLog, sizeof S().tLog, RLOG_TOPIC_ROOT "/%s/log", S().id);
  snprintf(S().tStatus, sizeof S().tStatus, RLOG_TOPIC_ROOT "/%s/status", S().id);
  snprintf(S().tCmd, sizeof S().tCmd, RLOG_TOPIC_ROOT "/%s/cmd", S().id);
  snprintf(S().fw, sizeof S().fw, "%s", fwVersion ? fwVersion : "");

  static const char kOffline[] = "{\"state\":\"offline\"}";
  const bool tls = strncmp(uri, "wss://", 6) == 0 || strncmp(uri, "mqtts://", 8) == 0;

  esp_mqtt_client_config_t cfg = {};
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.uri = uri;
  if (tls) {
    if (rootCaPem) cfg.broker.verification.certificate = rootCaPem;
    else cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.credentials.client_id = S().clientId;
  cfg.credentials.username = username;
  cfg.credentials.authentication.password = password;
  cfg.session.last_will.topic = S().tStatus;
  cfg.session.last_will.msg = kOffline;
  cfg.session.last_will.msg_len = sizeof(kOffline) - 1;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = 60;  // server marks the device offline ~90 s after it vanishes
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 15000;
  cfg.buffer.size = 1536;
  cfg.buffer.out_size = 2048;
  cfg.task.stack_size = 6144;
#else
  cfg.uri = uri;
  if (tls) {
    if (rootCaPem) cfg.cert_pem = rootCaPem;
    else cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.client_id = S().clientId;
  cfg.username = username;
  cfg.password = password;
  cfg.lwt_topic = S().tStatus;
  cfg.lwt_msg = kOffline;
  cfg.lwt_msg_len = sizeof(kOffline) - 1;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = 60;
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms = 15000;
  cfg.buffer_size = 1536;
  cfg.out_buffer_size = 2048;
  cfg.task_stack = 6144;
#endif

  S().client = esp_mqtt_client_init(&cfg);
  if (!S().client) return false;
  esp_mqtt_client_register_event(S().client, MQTT_EVENT_ANY, detail::onEvent, nullptr);
  xTaskCreate(detail::flushTask, "rlog", 3072, nullptr, 2, &S().task);
  return esp_mqtt_client_start(S().client) == ESP_OK;
}

/** Pause sending during heavy work (OTA, big uploads). Lines keep buffering. */
inline void pause(bool paused) {
  S().paused = paused;
  if (!paused && S().task) xTaskNotifyGive(S().task);
}

/**
 * Send everything buffered now and wait (up to timeoutMs, including time to
 * connect) until it has been handed to the socket. Use right before
 * ESP.restart(). Returns false if lines were still pending at the timeout.
 */
inline bool flush(uint32_t timeoutMs = 3000) {
  if (!S().client || !S().task) return false;
  S().paused = false;
  S().force = true;
  const uint32_t start = millis();
  bool done = false;
  while (millis() - start < timeoutMs) {
    xTaskNotifyGive(S().task);
    if (S().connected && S().len == 0 && !S().sending) {
      done = true;
      break;
    }
    delay(20);
  }
  S().force = false;
  if (done) delay(150);  // let lwIP put the last frames on the air
  return done;
}

inline bool connected() { return S().connected; }
inline Level remoteLevel() { return S().remote; }
inline Level serialLevel() { return S().serial; }
inline const char *deviceId() { return S().id; }

/** Core logger. Prefer the RLOGx macros below. */
inline void logv(Level level, const char *tag, const char *fmt, va_list ap) {
  if (level == OFF || level > DEBUG) return;
  const bool toSerial = level <= S().serial;
  const bool toRemote = level <= S().remote && S().lock;  // buffered even before begin()
  if (!toSerial && !toRemote) return;

  char line[kMaxLine];
  int head = snprintf(line, sizeof line, "[%c][%s] ", levelLetter(level), tag ? tag : "APP");
  if (head < 0) return;
  if (head >= static_cast<int>(sizeof line)) head = sizeof line - 1;
  int body = vsnprintf(line + head, sizeof line - head, fmt, ap);
  size_t len = static_cast<size_t>(head) + (body > 0 ? static_cast<size_t>(body) : 0);
  if (len > sizeof line - 1) len = sizeof line - 1;  // truncated
  for (size_t i = 0; i < len; ++i)
    if (line[i] == '\n' || line[i] == '\r') line[i] = ' ';  // one line per entry

  // Serial gets the line as-is (plus the optional local prefix); the server
  // adds the "[<MODEL>]" label to stored lines itself.
  if (toSerial) {
    Serial.printf("%s%s%.*s\n", RLOG_SERIAL_PREFIX, sizeof(RLOG_SERIAL_PREFIX) > 1 ? " " : "",
                  static_cast<int>(len), line);
  }
  if (toRemote) detail::enqueueLine(line, len);
}

inline void log(Level level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
inline void log(Level level, const char *tag, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  logv(level, tag, fmt, ap);
  va_end(ap);
}

}  // namespace RemoteLog

#define RLOGE(tag, fmt, ...) RemoteLog::log(RemoteLog::ERROR, tag, fmt, ##__VA_ARGS__)
#define RLOGW(tag, fmt, ...) RemoteLog::log(RemoteLog::WARN, tag, fmt, ##__VA_ARGS__)
#define RLOGI(tag, fmt, ...) RemoteLog::log(RemoteLog::INFO, tag, fmt, ##__VA_ARGS__)
#define RLOGD(tag, fmt, ...) RemoteLog::log(RemoteLog::DEBUG, tag, fmt, ##__VA_ARGS__)
