/* ===========================================================================
 * KUTSU Sense — WiFi link + KUTSU Hub client (see mesh.h for the behaviour)
 * ===========================================================================
 * Built-in libs only: WiFi, WiFiUdp, HTTPClient, Preferences, ArduinoJson.
 * Hub protocol (shared with ../../KutsuHub/kutsu_hub.py):
 *   UDP HUB_UDP_PORT  we send   {"kutsu_discover":1,"id":..,"mac":..}  (broadcast)
 *                     hub sends {"kutsu_hub":1,"v":1,"port":8080,"name":..,"time":<epoch>}
 *   POST /api/live             current record (latest reading, not stored)
 *   POST /api/ingest?src=log   NDJSON lines from our log (stored by the hub)
 *   POST /api/ingest           current record, stored (used when we have no log)
 * =========================================================================== */
#include "mesh.h"
#include "identity.h"
#include "kutsu_link.h"
#include "ble_provisioning.h"
#include "settings.h"
#include "config.h"

#include <WiFi.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <time.h>

static SemaphoreHandle_t s_mtx = nullptr;      // guards the Strings below
static String            s_ssid, s_pass;
static String            s_hubCfg;              // NVS "hub": "" = auto-discover
static String            s_hubAddr;             // "ip:port" in use, "" = none
static volatile bool     s_reprovision = false; // set by netProvision (loop/BLE task)
static volatile bool     s_rehub = false;       // set by netSetHub
static volatile NetState s_state = NET_NO_CREDS;
static volatile uint32_t s_nextTryMs = 0;       // NET_OFFLINE: millis of next attempt
static bool              s_retryScheduled = false;

static WiFiUDP           s_udp;
static bool              s_udpUp = false;
static uint32_t          s_hubLastOk = 0;       // millis of the last accepted push
static uint32_t          s_hubEpoch = 0;        // "time" from the last hub packet ...
static uint32_t          s_hubEpochAt = 0;      // ... and when we got it (millis)
static uint32_t          s_onlineAt = 0;        // millis when the current link came up
static int64_t           s_sentOff = -1;        // log bytes delivered (-1 = not started this boot)
static volatile uint32_t s_backlog = 0;
static char              s_batch[HUB_BATCH_BYTES];

static void lock()   { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock() { if (s_mtx) xSemaphoreGive(s_mtx); }

// ---- Credential persistence ---------------------------------------------------
// NVS wins once a key exists (even an empty ssid = "WiFi disabled"); otherwise
// fall back to the compile-time defaults in config.h.
static void loadCreds() {
  Preferences p;
  p.begin(NVS_NAMESPACE, /*readonly=*/true);
  bool   haveNvs = p.isKey("ssid");
  String ssid = haveNvs ? p.getString("ssid", "") : String(WIFI_SSID);
  String pass = haveNvs ? p.getString("pass", "") : String(WIFI_PASSWORD);
  String room = p.isKey("room") ? p.getString("room", "") : String(ROOM_LABEL);
  String hub  = p.isKey("hub")  ? p.getString("hub", "")  : String(HUB_ADDRESS);
  p.end();
  lock(); s_ssid = ssid; s_pass = pass; s_hubCfg = hub; unlock();
  kutsuSetRoom(room.c_str());
}

void netProvision(const char* ssid, const char* pass, const char* room) {
  Preferences p;
  p.begin(NVS_NAMESPACE, /*readonly=*/false);
  p.putString("ssid", ssid ? ssid : "");
  p.putString("pass", pass ? pass : "");
  if (room) p.putString("room", room);
  p.end();
  s_reprovision = true;                         // netTask reloads and reconnects
}

void netSetHub(const char* addr) {
  String a = addr ? String(addr) : String();
  a.trim();
  if (a.equalsIgnoreCase("auto")) a = "";
  Preferences p;
  p.begin(NVS_NAMESPACE, /*readonly=*/false);
  p.putString("hub", a);
  p.end();
  lock(); s_hubCfg = a; unlock();
  s_rehub = true;                               // netTask drops the current hub and re-resolves
}
String netHubSetting() { lock(); String s = s_hubCfg; unlock(); return s; }
String netHub()        { lock(); String s = s_hubAddr; unlock(); return s; }

// ---- Status ---------------------------------------------------------------------
NetState netState()          { return s_state; }
bool     netWifiConnected()  { return s_state == NET_ONLINE && WiFi.status() == WL_CONNECTED; }
String   netSsid()           { lock(); String s = s_ssid; unlock(); return s; }
String   netIp()             { return netWifiConnected() ? WiFi.localIP().toString() : String(); }
int      netRssi()           { return netWifiConnected() ? WiFi.RSSI() : 0; }
uint32_t netBacklogBytes()   { return s_backlog; }
uint32_t netRetryInS() {
  if (s_state != NET_OFFLINE || !s_retryScheduled) return 0;
  int32_t ms = (int32_t)(s_nextTryMs - millis());
  return ms > 0 ? (uint32_t)(ms + 999) / 1000 : 0;
}
const char* netStateStr(NetState s) {
  switch (s) {
    case NET_CONNECTING: return "connecting";
    case NET_ONLINE:     return "online";
    case NET_OFFLINE:    return "offline";
    default:             return "no wifi set";
  }
}

// ---- Hub discovery --------------------------------------------------------------
static void setHub(const String& addr) {
  lock(); bool changed = (s_hubAddr != addr); s_hubAddr = addr; unlock();
  if (!changed) return;
  if (addr.length()) {
    Serial.printf("[hub] using %s\n", addr.c_str());
    s_hubLastOk = millis();                     // grace period before declaring it lost
  } else {
    kutsuSetRole(ROLE_SOLO);
    kutsuSetMasterIp("");
  }
  bleProvisioningNotifyStatus();
}

// Drain pending UDP packets; adopt the first hub that speaks (auto mode) and
// keep its clock for the time-sync fallback.
static void readHubPackets(bool adopt) {
  if (!s_udpUp) return;
  for (int guard = 0; guard < 8; guard++) {
    int n = s_udp.parsePacket();
    if (n <= 0) return;
    char buf[384];
    int len = s_udp.read((uint8_t*)buf, sizeof(buf) - 1);
    if (len <= 0) continue;
    buf[len] = 0;
    JsonDocument d;
    if (deserializeJson(d, buf) || !(d["kutsu_hub"] | 0)) continue;   // our own probe, or noise
    uint32_t ep = d["time"] | 0UL;
    if (ep > 1600000000UL) { s_hubEpoch = ep; s_hubEpochAt = millis(); }
    if (adopt) {
      uint16_t port = d["port"] | (uint16_t)HUB_DEFAULT_PORT;
      setHub(s_udp.remoteIP().toString() + ":" + String(port));
      adopt = false;
    }
  }
}

static void sendProbe() {
  if (!s_udpUp) s_udpUp = s_udp.begin(HUB_UDP_PORT);
  if (!s_udpUp) return;
  String probe = String("{\"kutsu_discover\":1,\"id\":\"") + kutsuName() + "\",\"mac\":\"" + kutsuMacStr() + "\"}";
  s_udp.beginPacket(IPAddress(255, 255, 255, 255), HUB_UDP_PORT);
  s_udp.write((const uint8_t*)probe.c_str(), probe.length());
  s_udp.endPacket();
}

// Resolve the configured "host[:port]" (manual mode).
static bool resolveManualHub(const String& cfg) {
  String host = cfg; uint16_t port = HUB_DEFAULT_PORT;
  if (host.startsWith("http://")) host = host.substring(7);
  int slash = host.indexOf('/'); if (slash >= 0) host = host.substring(0, slash);
  int colon = host.lastIndexOf(':');
  if (colon > 0) { port = (uint16_t)host.substring(colon + 1).toInt(); host = host.substring(0, colon); }
  IPAddress ip;
  if (!ip.fromString(host) && !WiFi.hostByName(host.c_str(), ip)) {
    Serial.printf("[hub] cannot resolve \"%s\"\n", host.c_str());
    return false;
  }
  setHub(ip.toString() + ":" + String(port ? port : HUB_DEFAULT_PORT));
  return true;
}

static void findHub() {
  static uint32_t lastNote = 0;
  String cfg = netHubSetting();
  if (cfg.length()) { resolveManualHub(cfg); return; }
  sendProbe();
  uint32_t t0 = millis();
  while (!netHub().length() && millis() - t0 < 1500) {   // answers arrive within ms on a LAN
    readHubPackets(true);
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  if (!netHub().length() && (lastNote == 0 || millis() - lastNote > 30000)) {
    lastNote = millis();
    Serial.printf("[hub] no hub answered on UDP %d (udp=%s). Is KUTSU Hub running on this network? "
                  "If broadcasts are blocked, pin it: hub <ip:port>\n", HUB_UDP_PORT, s_udpUp ? "ok" : "FAILED");
  }
}

// ---- Hub push -------------------------------------------------------------------
static int hubPost(const char* path, const uint8_t* data, size_t len, const char* ctype) {
  String hub = netHub();
  if (!hub.length()) return -1;
  HTTPClient http; WiFiClient client;
  if (!http.begin(client, "http://" + hub + path)) return -1;
  http.setTimeout(4000);
  http.addHeader("Content-Type", ctype);
  int code = http.POST((uint8_t*)data, len);
  http.end();
  return code;
}
static bool ok2xx(int code) { return code >= 200 && code < 300; }

// Send NEW log lines (since s_sentOff) in whole-line batches. Returns false if
// the hub rejected/failed a batch (stop for this beat; retry next beat).
static bool pushLogTail() {
  if (!kutsuLogEnabled()) {                     // no SD/flash: let the hub store the live reading
    String rec = kutsuCurrentRecordJson();
    return ok2xx(hubPost("/api/ingest", (const uint8_t*)rec.c_str(), rec.length(), "application/json"));
  }
  size_t size = kutsuLogByteSize();
  // Start at the CURRENT end the first time a hub is reached in this boot (and only
  // once the clock is set, so every line we send carries a real `ts`).
  if (s_sentOff < 0) {
    if (!kutsuTimeSynced()) return true;
    s_sentOff = (int64_t)size;
    Serial.printf("[hub] streaming log from byte %u\n", (unsigned)size);
  }
  if ((int64_t)size < s_sentOff) s_sentOff = 0; // flash log rotated / file replaced: new file from its start
  for (int b = 0; b < HUB_MAX_BATCHES && s_sentOff < (int64_t)size; b++) {
    size_t n = kutsuLogReadChunk((size_t)s_sentOff, s_batch, sizeof(s_batch));
    if (n == 0) break;
    size_t len = n;
    while (len > 0 && s_batch[len - 1] != '\n') len--;   // whole lines only
    if (len == 0) {
      if (n == sizeof(s_batch)) { s_sentOff += n; continue; }  // a line longer than a batch: skip it
      break;                                    // a line still being written: next beat
    }
    if (!ok2xx(hubPost("/api/ingest?src=log", (const uint8_t*)s_batch, len, "application/x-ndjson"))) return false;
    s_sentOff += len;
  }
  s_backlog = (s_sentOff >= 0 && (int64_t)size > s_sentOff) ? (uint32_t)(size - s_sentOff) : 0;
  return true;
}

static void hubTick() {
  bool autoMode = !netHubSetting().length();
  readHubPackets(autoMode && !netHub().length());
  if (!netHub().length()) findHub();
  if (!netHub().length()) return;

  // Clock fallback: a LAN without internet never answers NTP, so take the hub's.
  if (!kutsuTimeSynced() && s_hubEpoch && millis() - s_onlineAt > HUB_NTP_GRACE_MS)
    kutsuApplyTimeSync(s_hubEpoch + (millis() - s_hubEpochAt) / 1000, "hub");

  static uint32_t lastErrNote = 0;
  String rec = kutsuCurrentRecordJson();
  int code = hubPost("/api/live", (const uint8_t*)rec.c_str(), rec.length(), "application/json");
  bool ok = ok2xx(code);
  if (!ok && (lastErrNote == 0 || millis() - lastErrNote > 15000)) {
    lastErrNote = millis();
    // <0 = HTTPClient error (-1 refused/unreachable, -11 timeout ...); 4xx/5xx = hub answered with an error
    Serial.printf("[hub] POST http://%s/api/live failed: %d (%s)\n", netHub().c_str(), code,
                  code < 0 ? HTTPClient::errorToString(code).c_str() : "HTTP status");
  }
  if (ok) ok = pushLogTail();
  if (ok) {
    s_hubLastOk = millis();
    if (kutsuGetRole() != ROLE_SLAVE) {
      kutsuSetRole(ROLE_SLAVE);
      kutsuSetMasterIp(netHub().c_str());
      Serial.printf("[hub] connected: dashboard at http://%s/\n", netHub().c_str());
      bleProvisioningNotifyStatus();
    }
  } else if (millis() - s_hubLastOk > HUB_TIMEOUT_MS) {
    Serial.printf("[hub] %s not answering; %s\n", netHub().c_str(), autoMode ? "rediscovering" : "retrying");
    kutsuSetRole(ROLE_SOLO);
    if (autoMode) setHub("");
    else          s_hubLastOk = millis();      // manual: keep the address, keep trying
  }
}

// Leave the network side entirely: back to a stand-alone unit. (s_sentOff is
// kept, so the backlog is delivered when the hub is reachable again.)
static void goOffline() {
  if (s_udpUp) { s_udp.stop(); s_udpUp = false; }
  setHub("");
  kutsuSetRole(ROLE_SOLO);
  kutsuSetMasterIp("");
  WiFi.disconnect(false);                       // stay in STA mode for passive scans
}

static bool connectWifi() {
  String ssid, pass;
  lock(); ssid = s_ssid; pass = s_pass; unlock();
  // Own the radio for the attempt: the core stops kicking passive scans, and
  // we wait out one that is already running (it would make begin() fail).
  if (!kutsuRadioTake(WIFI_CONNECT_MS)) return false;
  uint32_t t0 = millis();
  while (WiFi.scanComplete() == WIFI_SCAN_RUNNING && millis() - t0 < 8000) vTaskDelay(pdMS_TO_TICKS(100));

  Serial.printf("[net] joining \"%s\"...\n", ssid.c_str());
  WiFi.setAutoReconnect(false);                 // retries are paced by netTask
  WiFi.begin(ssid.c_str(), pass.c_str());
  t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS) vTaskDelay(pdMS_TO_TICKS(200));
  bool ok = (WiFi.status() == WL_CONNECTED);
  if (!ok) WiFi.disconnect(false);
  kutsuRadioGive();

  if (ok) Serial.printf("[net] WiFi up: %s  rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  else    Serial.println("[net] WiFi join failed -> stand-alone");
  return ok;
}

static void scheduleRetry() {
  s_retryScheduled = (WIFI_RETRY_MS > 0);
  s_nextTryMs = millis() + WIFI_RETRY_MS;
  s_state = NET_OFFLINE;
}

static void netTask(void* pv) {
  loadCreds();

  for (;;) {
    if (s_reprovision) {                        // new creds over serial/BLE
      s_reprovision = false;
      if (s_state == NET_ONLINE) Serial.println("[net] reprovisioned; leaving network");
      goOffline();
      s_retryScheduled = false;
      s_state = NET_NO_CREDS;
      loadCreds();
    }
    if (s_rehub) { s_rehub = false; setHub(""); }

    // HANDHELD never joins a network: even with saved credentials, a handheld
    // unit stays off WiFi (BLE/USB only). Switching to STATIONARY resumes below.
    if (settingsMode() == MODE_HANDHELD) {
      if (s_state == NET_ONLINE || WiFi.status() == WL_CONNECTED) {
        Serial.println("[net] handheld mode -> staying off WiFi");
        goOffline();
      }
      s_retryScheduled = false;
      s_state = NET_NO_CREDS;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (!netSsid().length()) {                  // nothing configured: stand-alone
      s_state = NET_NO_CREDS;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (s_state == NET_ONLINE) {              // link dropped while online:
        Serial.println("[net] link lost -> stand-alone");
        goOffline();                            // fall through to one immediate rejoin attempt
      }
      if (s_state == NET_OFFLINE) {
        if (!s_retryScheduled || (int32_t)(millis() - s_nextTryMs) < 0) {
          vTaskDelay(pdMS_TO_TICKS(500));       // waiting for the retry timer (or never)
          continue;
        }
      }
      s_state = NET_CONNECTING;
      if (!connectWifi()) { scheduleRetry(); continue; }
      configTime(0, 0, NTP_SERVER_1, NTP_SERVER_2);   // UTC epoch; SNTP runs in the background
      s_state = NET_ONLINE;
      s_onlineAt = millis();
      bleProvisioningNotifyStatus();            // push the fresh IP to a connected app
    }

    if (!kutsuTimeSynced()) {
      time_t now = time(nullptr);
      if (now > 1700000000) kutsuApplyTimeSync((uint32_t)now);   // sane epoch => NTP answered
    }

    hubTick();

    // Sleep one push interval, but wake promptly for reprovisioning, a hub
    // change, link loss, or a switch to handheld mode.
    for (uint32_t t = 0; t < HUB_PUSH_MS && !s_reprovision && !s_rehub
                         && WiFi.status() == WL_CONNECTED && settingsMode() == MODE_STATIONARY; t += 250)
      vTaskDelay(pdMS_TO_TICKS(250));
  }
}

void netStart() {
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
  xTaskCreate(netTask, "netTask", 8192, nullptr, 4, nullptr);
}
