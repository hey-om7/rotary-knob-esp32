/**
 * RemoteLog.h — Knob Controller remote logging for FirmwareManager
 * (MQTT over WebSocket).
 *
 * Synced with examples/esp32/RemoteLogExample/RemoteLog.h (per-channel level
 * sets). Additions for this sketch:
 *   - init(): call at the very top of setup() so lines logged before Wi-Fi is
 *     up (boot, Wi-Fi portal, ...) are buffered and sent once MQTT connects.
 *   - flush(): push buffered lines out before ESP.restart() (OTA, /api/restart).
 *   - RLOG_SERIAL_PREFIX: optional local-only Serial prefix (default none).
 *   - connection supervision (below).
 *
 * Connects to the broker built into the FirmwareManager backend and:
 *   - ships log lines in batches to   fm/<mac>/log     (QoS 1)
 *   - reports fw + levels on          fm/<mac>/status  (retained; LWT = offline)
 *   - applies log-level commands from fm/<mac>/cmd     (retained, from the console)
 *   - sends a liveness ping "1" to    fm/<mac>/ping    (QoS 1, every 20 s)
 *
 * Connection supervision — recovers after a server restart without a reboot.
 * All of it runs in the flush task (never the MQTT event handler, which only
 * records timestamps / error codes), so esp-mqtt is never blocked:
 *   1. Liveness: while connected, ping every 20 s. Any PUBACK / SUBACK / DATA
 *      counts as "heard from server". Silent for 45 s -> disconnect + reconnect
 *      (catches half-open TCP after the server restarts / Render redeploys).
 *   2. MQTT keepalive 30 s.
 *   3. Offline for 2 min of retries with Wi-Fi up -> destroy and rebuild the
 *      client with the same config (fresh transport / TLS state); repeats
 *      every 2 min while still offline.
 *   4. MQTT_EVENT_ERROR -> error_type, esp_tls_last_esp_err,
 *      esp_transport_sock_errno, connect_return_code and the largest free
 *      8-bit heap block are printed to Serial.
 *   5. Wi-Fi got an IP again -> reconnect immediately.
 *   6. Last resort: offline > 10 min with Wi-Fi up -> reason saved in NVS,
 *      logged, ESP.restart(). The reason is logged again after the reboot.
 *      To avoid a reboot loop this only fires if MQTT has connected at least
 *      once since boot, and not while the broker is actively refusing us
 *      (wrong password / unregistered device: a restart can't fix that).
 *   Time spent paused (OTA) or with Wi-Fi down does not count toward 3 and 6.
 *
 * <mac> is the Wi-Fi STA MAC as 12 lowercase hex digits; client id "fm-<mac>".
 * The device must be registered in the console (Devices) with that MAC.
 * Works for any model: the server labels each line with the device's model
 * number ("###Logged msg: [<MODEL>] [I][TAG] text").
 *
 * Transport: ESP-IDF's built-in MQTT client (esp-mqtt) — supports wss://,
 * reconnects on its own and runs in its own task. No extra library needed.
 * Works on arduino-esp32 core 2.0.x (ESP-IDF 4.4) and 3.x (ESP-IDF 5.x).
 *
 *   Render / HTTPS host : "wss://<your-service>.onrender.com/mqtt"
 *   Local dev           : "ws://<pc-ip>:8080/mqtt"
 *   Self-hosted TCP     : "mqtt://<host>:1883"
 */
#pragma once

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <mqtt_client.h>

// Declared directly: WiFiClientSecure ships its own "esp_crt_bundle.h" that
// can shadow ESP-IDF's. This symbol is the IDF one, with the full Mozilla CA
// bundle compiled into the core (verifies Render's / Let's Encrypt / Google
// Trust Services certificates without pasting a PEM).
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);

// Optional local-only Serial prefix, e.g. "[KNOB]". Never sent to the server.
#ifndef RLOG_SERIAL_PREFIX
#define RLOG_SERIAL_PREFIX ""
#endif

namespace RemoteLog {

enum Level : uint8_t { OFF = 0, ERROR = 1, WARN = 2, INFO = 3, DEBUG = 4 };

/**
 * Each channel (remote, serial) has a SET of enabled levels, as a bitmask —
 * any combination, e.g. only ERROR + DEBUG. 0 = channel off.
 */
static constexpr uint8_t bitOf(Level l) { return (l >= ERROR && l <= DEBUG) ? static_cast<uint8_t>(1u << (l - 1)) : 0; }
static constexpr uint8_t kAllLevels = 0x0F;

// ----------------------------------------------------------------------------
// Tunables
// ----------------------------------------------------------------------------
static constexpr size_t kMaxLine = 256;          // server accepts up to 1024
static constexpr size_t kBatchBytes = 1024;      // one MQTT publish per batch
static constexpr uint32_t kFlushMs = 2000;       // max delay before a batch is sent
static constexpr uint8_t kDefaultRemote = 0x07;  // error + warn + info
static constexpr uint8_t kDefaultSerial = kAllLevels;  // USB serial: everything

// Connection supervision (see header comment)
static constexpr int kKeepaliveSec = 30;
static constexpr uint32_t kPingMs = 20000;           // liveness ping interval while connected
static constexpr uint32_t kSilentMs = 45000;         // nothing heard for this long -> reconnect
static constexpr uint32_t kReconnectRetryMs = 5000;  // keep retrying reconnect() this long after a kick
static constexpr uint32_t kRebuildMs = 120000;       // offline (Wi-Fi up) -> rebuild client, repeat
static constexpr uint32_t kRestartMs = 600000;       // offline (Wi-Fi up) -> ESP.restart()
static constexpr uint32_t kTaskStack = 4096;         // flush task (was 3072): + NVS, printf, rebuild

// ----------------------------------------------------------------------------
// State. Header-only and C++11-safe (core 2.x builds with gnu++11): a
// function-local static in an inline function is one instance program-wide.
// ----------------------------------------------------------------------------
struct State {
  esp_mqtt_client_handle_t client = nullptr;
  volatile bool connected = false;
  volatile bool paused = false;
  volatile bool force = false;    // flush(): send now, ignore batching delay
  volatile bool sending = false;  // a batch was taken and is being published
  volatile uint8_t remote = kDefaultRemote;  // level bitmask
  volatile uint8_t serial = kDefaultSerial;  // level bitmask
  SemaphoreHandle_t lock = nullptr;
  TaskHandle_t task = nullptr;
  char buf[kBatchBytes + 1];
  size_t len = 0;
  uint32_t firstAt = 0;  // millis() of the oldest buffered line
  uint32_t dropped = 0;  // lines discarded while offline / overflowing
  char id[13];
  char clientId[16];
  char tLog[24], tStatus[24], tCmd[24], tPing[24];
  char fw[33];

  // --- kept for client rebuilds (esp_mqtt_client_init copies, but we re-init) ---
  esp_mqtt_client_config_t cfg;  // zero-initialised (static storage)
  char uri[128], user[48], pass[64];

  // --- supervision; written by the flush task unless marked volatile ---
  volatile uint32_t lastHeard = 0;  // millis() of last CONNACK/PUBACK/SUBACK/DATA (MQTT task)
  volatile bool gotIp = false;      // set by the Wi-Fi event task
  volatile bool everConnected = false;
  volatile bool refused = false;    // last error was CONNECTION_REFUSED (cleared on connect)
  uint32_t lastPing = 0;
  bool offline = false;             // in an offline-with-Wi-Fi-up period
  uint32_t offlineSince = 0;
  uint32_t rebuildAt = 0;           // start of the current 2-minute rebuild window
  bool reconnectPending = false;
  uint32_t reconnectUntil = 0;

  // --- last MQTT_EVENT_ERROR: captured by the handler, printed by the flush task ---
  volatile bool errPending = false;
  volatile uint32_t errCount = 0;
  int errType = 0, errSock = 0, errRc = 0;
  esp_err_t errTls = ESP_OK;
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

// Defined at the bottom; used by the supervision code to log its decisions.
inline void log(Level level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

// ----------------------------------------------------------------------------
// Internals
// ----------------------------------------------------------------------------
namespace detail {

/** Match a level name at p (followed by a closing quote); 0 if none. */
inline uint8_t nameBit(const char *p) {
  for (uint8_t i = ERROR; i <= DEBUG; ++i) {
    const char *name = levelName(static_cast<Level>(i));
    size_t n = strlen(name);
    if (strncmp(p, name, n) == 0 && p[n] == '"') return bitOf(static_cast<Level>(i));
  }
  return 0;
}

/**
 * Read "key" from a tiny flat JSON object as a level bitmask:
 *   "key":["error","debug"]  -> any combination ([] = off)
 *   "key":"info"             -> legacy threshold (error+warn+info)
 * Returns false if the key is missing or malformed.
 */
inline bool jsonLevels(const char *json, const char *key, uint8_t &out) {
  char pat[16];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return false;
  p = strchr(p + strlen(pat), ':');
  if (!p) return false;
  ++p;
  while (*p == ' ') ++p;
  if (*p == '[') {
    const char *end = strchr(p, ']');
    if (!end) return false;
    uint8_t mask = 0;
    for (const char *q = strchr(p, '"'); q && q < end; q = strchr(q + 1, '"')) {
      mask |= nameBit(q + 1);
      q = strchr(q + 1, '"');  // skip to this name's closing quote
      if (!q) break;
    }
    out = mask;
    return true;
  }
  if (*p == '"') {
    ++p;
    if (strncmp(p, "off\"", 4) == 0) {
      out = 0;
      return true;
    }
    uint8_t b = nameBit(p);
    if (!b) return false;
    out = static_cast<uint8_t>((b << 1) - 1);  // threshold: this level and every more severe one
    return true;
  }
  return false;
}

/** Bitmask -> JSON array, e.g. ["error","info"]. */
inline int levelsJson(char *out, size_t cap, uint8_t mask) {
  int n = snprintf(out, cap, "[");
  for (uint8_t i = ERROR; i <= DEBUG; ++i) {
    if (!(mask & bitOf(static_cast<Level>(i)))) continue;
    n += snprintf(out + n, cap - n, "%s\"%s\"", n > 1 ? "," : "", levelName(static_cast<Level>(i)));
  }
  n += snprintf(out + n, cap - n, "]");
  return n;
}

/** Retained status: the server shows these as the device's applied levels. */
inline void publishStatus(esp_mqtt_client_handle_t c) {
  if (!c) return;
  char r[48], s[48], json[160];
  levelsJson(r, sizeof r, S().remote);
  levelsJson(s, sizeof s, S().serial);
  int n = snprintf(json, sizeof json, "{\"state\":\"online\",\"fw\":\"%s\",\"remote\":%s,\"serial\":%s}", S().fw, r, s);
  // enqueue = non-blocking, safe from any task including the MQTT event handler.
  esp_mqtt_client_enqueue(c, S().tStatus, json, n, 1, 1, true);
}

inline void saveLevels() {
  Preferences p;
  if (p.begin("rlog", false)) {
    p.putUChar("rmask", S().remote);
    p.putUChar("smask", S().serial);
    p.end();
  }
}

/**
 * Loads levels and takes (reads + clears) the reason saved before a watchdog
 * restart, if any. Read-write so the namespace is created on first boot (a
 * read-only open of a missing namespace prints an NVS error on every boot).
 */
inline void loadLevels(char *restartNote, size_t noteCap) {
  restartNote[0] = '\0';
  Preferences p;
  if (p.begin("rlog", false)) {
    S().remote = p.getUChar("rmask", kDefaultRemote) & kAllLevels;
    S().serial = p.getUChar("smask", kDefaultSerial) & kAllLevels;
    if (p.isKey("rst")) {
      p.getString("rst", restartNote, noteCap);
      p.remove("rst");
    }
    p.end();
  }
}

inline void saveRestartNote(const char *note) {
  Preferences p;
  if (p.begin("rlog", false)) {
    p.putString("rst", note);
    p.end();
  }
}

/** {"remote":["error","warn"],"serial":[]} — keys optional. Empty = cleared, ignore. */
inline void applyCommand(esp_mqtt_client_handle_t c, const char *json) {
  uint8_t r = S().remote, s = S().serial;
  // Parse both keys (no short-circuit): either one may be present.
  const bool hasRemote = jsonLevels(json, "remote", r);
  const bool hasSerial = jsonLevels(json, "serial", s);
  if (!hasRemote && !hasSerial) return;
  bool changed = (r != S().remote) || (s != S().serial);
  S().remote = r;
  S().serial = s;
  if (changed) saveLevels();
  publishStatus(c);  // always confirm, so the console shows "synced"
}

/**
 * Runs in the esp-mqtt task: only cheap bookkeeping, no blocking calls.
 * Uses the event's own client handle (S().client is nulled during a rebuild).
 */
inline void onEvent(void *, esp_event_base_t, int32_t id, void *data) {
  auto *e = static_cast<esp_mqtt_event_handle_t>(data);
  switch (static_cast<esp_mqtt_event_id_t>(id)) {
    case MQTT_EVENT_CONNECTED:
      S().connected = true;
      S().everConnected = true;
      S().refused = false;
      S().lastHeard = millis();
#if ESP_IDF_VERSION_MAJOR >= 5
      esp_mqtt_client_subscribe_single(e->client, S().tCmd, 1);
#else
      esp_mqtt_client_subscribe(e->client, S().tCmd, 1);
#endif
      publishStatus(e->client);
      if (S().task) xTaskNotifyGive(S().task);  // send whatever piled up
      break;
    case MQTT_EVENT_DISCONNECTED:
      S().connected = false;
      if (S().task) xTaskNotifyGive(S().task);
      break;
    case MQTT_EVENT_PUBLISHED:
    case MQTT_EVENT_SUBSCRIBED:
      S().lastHeard = millis();
      break;
    case MQTT_EVENT_DATA: {
      S().lastHeard = millis();
      // Commands are tiny (server caps them at 96 bytes) and never fragmented.
      size_t tl = strlen(S().tCmd);
      if (e->topic_len == static_cast<int>(tl) && strncmp(e->topic, S().tCmd, tl) == 0 && e->current_data_offset == 0 &&
          e->data_len == e->total_data_len && e->data_len > 0 && e->data_len < 128) {
        char cmd[128];
        memcpy(cmd, e->data, e->data_len);
        cmd[e->data_len] = '\0';
        applyCommand(e->client, cmd);
      }
      break;
    }
    case MQTT_EVENT_ERROR:
      if (e->error_handle) {
        S().errType = static_cast<int>(e->error_handle->error_type);
        S().errTls = e->error_handle->esp_tls_last_esp_err;
        S().errSock = e->error_handle->esp_transport_sock_errno;
        S().errRc = static_cast<int>(e->error_handle->connect_return_code);
        S().refused = e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED;
      }
      ++S().errCount;
      S().errPending = true;
      if (S().task) xTaskNotifyGive(S().task);
      break;
    default:
      break;
  }
}

/** Arduino Wi-Fi event task: just flag it; the flush task does the reconnect. */
inline void onWifiGotIp(arduino_event_id_t) {
  S().gotIp = true;
  if (S().task) xTaskNotifyGive(S().task);
}

inline void printError() {
  S().errPending = false;
  Serial.printf("[E][RLOG] MQTT error #%u: type=%d tls_err=0x%x sock_errno=%d connect_rc=%d largest_free=%u\n",
                static_cast<unsigned>(S().errCount), S().errType, static_cast<unsigned>(S().errTls), S().errSock,
                S().errRc, static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
}

/** init + register + start with the stored config. Flush task / begin() only. */
inline bool startClient() {
  S().client = esp_mqtt_client_init(&S().cfg);
  if (!S().client) return false;
  esp_mqtt_client_register_event(S().client, MQTT_EVENT_ANY, onEvent, nullptr);
  return esp_mqtt_client_start(S().client) == ESP_OK;
}

/** Full teardown + rebuild for a fresh transport / TLS state. Flush task only. */
inline void rebuildClient() {
  log(WARN, "RLOG", "MQTT offline %us with Wi-Fi up - rebuilding client (largest_free=%u)",
      static_cast<unsigned>((millis() - S().offlineSince) / 1000),
      static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
  S().connected = false;
  S().reconnectPending = false;
  if (S().client) {
    esp_mqtt_client_handle_t old = S().client;
    S().client = nullptr;  // publishStatus() and friends skip a null client
    esp_mqtt_client_stop(old);
    esp_mqtt_client_destroy(old);
  }
  S().connected = false;  // the old task may have flipped it while stopping
  if (!startClient()) Serial.printf("[E][RLOG] MQTT client rebuild failed, retry in %us\n", (unsigned)(kRebuildMs / 1000));
}

/** Ask esp-mqtt to (re)connect now; retried each tick until it accepts. */
inline void kickReconnect(uint32_t now) {
  S().reconnectPending = true;
  S().reconnectUntil = now + kReconnectRetryMs;
}

/**
 * Connection supervision, once per flush-task wake-up (<= every 250 ms).
 * A handful of comparisons in the common case; client calls are rare.
 */
inline void supervise() {
  const uint32_t now = millis();
  if (S().errPending) printError();

  if (S().gotIp) {
    S().gotIp = false;
    if (S().client && !S().connected) kickReconnect(now);
  }

  // esp_mqtt_client_reconnect() returns ESP_FAIL until the client has reached
  // its wait-reconnect state (e.g. right after disconnect()), so keep trying
  // briefly. If it never accepts, esp-mqtt's own 5 s auto-reconnect still runs.
  if (S().reconnectPending) {
    if (S().connected || (S().client && esp_mqtt_client_reconnect(S().client) == ESP_OK) ||
        static_cast<int32_t>(now - S().reconnectUntil) >= 0)
      S().reconnectPending = false;
  }

  if (S().paused || !S().client) return;

  if (S().connected) {
    S().offline = false;
    // 1. Liveness ping. enqueue = non-blocking; the MQTT task sends it.
    if (now - S().lastPing >= kPingMs) {
      S().lastPing = now;
      esp_mqtt_client_enqueue(S().client, S().tPing, "1", 1, 1, 0, true);
    }
    if (now - S().lastHeard >= kSilentMs) {
      log(WARN, "RLOG", "No reply from server for %us - forcing MQTT reconnect",
          static_cast<unsigned>((now - S().lastHeard) / 1000));
      S().connected = false;  // stop publishing into a dead link right away
      S().lastHeard = now;
      esp_mqtt_client_disconnect(S().client);
      kickReconnect(now);
    }
    return;
  }

  // Offline: only count time while Wi-Fi is actually up.
  if (WiFi.status() != WL_CONNECTED) {
    S().offline = false;
    return;
  }
  if (!S().offline) {
    S().offline = true;
    S().offlineSince = now;
    S().rebuildAt = now;
    return;
  }

  // 3. Rebuild after 2 min of failed retries, then every 2 min.
  if (now - S().rebuildAt >= kRebuildMs) {
    S().rebuildAt = now;
    rebuildClient();
    return;
  }

  // 6. Last resort.
  if (now - S().offlineSince >= kRestartMs && S().everConnected && !S().refused) {
    char note[96];
    snprintf(note, sizeof note, "MQTT offline %us with Wi-Fi up (errors=%u, last type=%d sock_errno=%d)",
             static_cast<unsigned>((now - S().offlineSince) / 1000), static_cast<unsigned>(S().errCount),
             S().errType, S().errSock);
    saveRestartNote(note);
    log(ERROR, "RLOG", "Restarting: %s", note);
    Serial.flush();
    delay(100);
    ESP.restart();
  }
}

/** Sends buffered batches and supervises the connection. Never holds S().lock while calling into esp-mqtt. */
inline void flushTask(void *) {
  static char out[kBatchBytes + 1];
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
    supervise();
    if (!S().connected || S().paused || !S().client) continue;

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
  char note[96];
  detail::loadLevels(note, sizeof note);
  S().lock = xSemaphoreCreateMutex();
  if (note[0]) log(WARN, "RLOG", "Previous restart by RemoteLog watchdog: %s", note);
}

/**
 * Start the MQTT client. Call once Wi-Fi is connected (calling again is a
 * no-op). The client reconnects by itself after Wi-Fi or server drops.
 *
 * uri / username / password are copied. rootCaPem (optional PEM to pin a CA;
 * nullptr = built-in CA bundle, recommended for Render) is NOT copied and must
 * stay valid for the program's lifetime, since the client may be rebuilt.
 */
inline bool begin(const char *uri, const char *username, const char *password, const char *fwVersion,
                  const char *rootCaPem = nullptr) {
  if (S().client) return true;
  init();
  if (!S().lock) return false;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(S().id, sizeof S().id, "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  snprintf(S().clientId, sizeof S().clientId, "fm-%s", S().id);
  snprintf(S().tLog, sizeof S().tLog, "fm/%s/log", S().id);
  snprintf(S().tStatus, sizeof S().tStatus, "fm/%s/status", S().id);
  snprintf(S().tCmd, sizeof S().tCmd, "fm/%s/cmd", S().id);
  snprintf(S().tPing, sizeof S().tPing, "fm/%s/ping", S().id);
  snprintf(S().fw, sizeof S().fw, "%s", fwVersion ? fwVersion : "");
  snprintf(S().uri, sizeof S().uri, "%s", uri ? uri : "");
  snprintf(S().user, sizeof S().user, "%s", username ? username : "");
  snprintf(S().pass, sizeof S().pass, "%s", password ? password : "");

  static const char kOffline[] = "{\"state\":\"offline\"}";
  const bool tls = strncmp(S().uri, "wss://", 6) == 0 || strncmp(S().uri, "mqtts://", 8) == 0;

  esp_mqtt_client_config_t &cfg = S().cfg;
  cfg = esp_mqtt_client_config_t();
#if ESP_IDF_VERSION_MAJOR >= 5
  cfg.broker.address.uri = S().uri;
  if (tls) {
    if (rootCaPem) cfg.broker.verification.certificate = rootCaPem;
    else cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.credentials.client_id = S().clientId;
  cfg.credentials.username = S().user;
  cfg.credentials.authentication.password = S().pass;
  cfg.session.last_will.topic = S().tStatus;
  cfg.session.last_will.msg = kOffline;
  cfg.session.last_will.msg_len = sizeof(kOffline) - 1;
  cfg.session.last_will.qos = 1;
  cfg.session.last_will.retain = 1;
  cfg.session.keepalive = kKeepaliveSec;
  cfg.network.reconnect_timeout_ms = 5000;
  cfg.network.timeout_ms = 15000;
  cfg.buffer.size = 1536;
  cfg.buffer.out_size = 2048;
  cfg.task.stack_size = 6144;
#else
  cfg.uri = S().uri;
  if (tls) {
    if (rootCaPem) cfg.cert_pem = rootCaPem;
    else cfg.crt_bundle_attach = esp_crt_bundle_attach;
  }
  cfg.client_id = S().clientId;
  cfg.username = S().user;
  cfg.password = S().pass;
  cfg.lwt_topic = S().tStatus;
  cfg.lwt_msg = kOffline;
  cfg.lwt_msg_len = sizeof(kOffline) - 1;
  cfg.lwt_qos = 1;
  cfg.lwt_retain = 1;
  cfg.keepalive = kKeepaliveSec;
  cfg.reconnect_timeout_ms = 5000;
  cfg.network_timeout_ms = 15000;
  cfg.buffer_size = 1536;
  cfg.out_buffer_size = 2048;
  cfg.task_stack = 6144;
#endif

  WiFi.onEvent(detail::onWifiGotIp, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  // Start the client before the task so the task never sees a half-built handle.
  const bool ok = detail::startClient();
  xTaskCreate(detail::flushTask, "rlog", kTaskStack, nullptr, 2, &S().task);
  return ok;
}

/** Pause sending during heavy work (OTA, big uploads). Lines keep buffering. */
inline void pause(bool paused) {
  if (!paused) {
    // Paused time must not count as silence / offline time.
    S().lastHeard = millis();
    S().offline = false;
  }
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
  pause(false);
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
/** Enabled levels as bitmasks (test with RemoteLog::bitOf(RemoteLog::INFO)). */
inline uint8_t remoteLevels() { return S().remote; }
inline uint8_t serialLevels() { return S().serial; }
inline const char *deviceId() { return S().id; }
/** Unused stack of the flush/supervisor task in bytes (0 before begin()). */
inline unsigned taskStackFree() {
  return S().task ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(S().task)) * sizeof(StackType_t) : 0;
}

/** Core logger. Prefer the RLOGx macros below. */
inline void logv(Level level, const char *tag, const char *fmt, va_list ap) {
  const uint8_t bit = bitOf(level);
  if (!bit) return;
  const bool toSerial = (S().serial & bit) != 0;
  const bool toRemote = (S().remote & bit) != 0 && S().lock;  // buffered even before begin()
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
