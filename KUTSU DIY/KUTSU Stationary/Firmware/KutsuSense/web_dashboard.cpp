/* ===========================================================================
 * KUTSU Sense — master-node LAN dashboard + ingest API
 * ===========================================================================
 * Uses the ESP32 core's SYNCHRONOUS WebServer (WebServer.h) — NOT
 * ESPAsyncWebServer/AsyncTCP. On ESP-IDF 5.x the async stack calls the raw
 * lwIP tcp_* API without holding the TCPIP core lock and trips
 * "assert failed: tcp_alloc ... Required to lock TCPIP core functionality!".
 * The core WebServer goes through the lwIP sockets API (which locks internally),
 * so it is safe. Live updates use client-side polling of /api/nodes every 2 s
 * (ample for 1 Hz room data) instead of a WebSocket.
 *
 * A dedicated task pumps server.handleClient(); the registry is shared with
 * netTask (own record) and the HTTP handlers, guarded by a mutex.
 * =========================================================================== */
#include "web_dashboard.h"
#include "identity.h"
#include "kutsu_link.h"
#include "config.h"
#include "index_html.h"

#include <WebServer.h>
#include <ArduinoJson.h>

struct NodeState {
  bool     used = false;
  String   id, mac, room, ip;
  uint32_t lastSeen = 0;     // millis of last record
  String   last;             // most recent record JSON
};

static WebServer         s_server(WEB_PORT);
static NodeState         s_nodes[MESH_MAX_NODES];
static SemaphoreHandle_t s_mtx = nullptr;
static volatile bool     s_running = false;
static TaskHandle_t      s_task = nullptr;

static void lock()   { if (s_mtx) xSemaphoreTake(s_mtx, portMAX_DELAY); }
static void unlock() { if (s_mtx) xSemaphoreGive(s_mtx); }

// Upsert by id (fallback mac); returns slot or nullptr if the table is full.
static NodeState* upsert(const String& id, const String& mac) {
  NodeState* freeSlot = nullptr;
  for (auto& n : s_nodes) {
    if (n.used && (n.id == id || (id.length() == 0 && n.mac == mac))) return &n;
    if (!n.used && !freeSlot) freeSlot = &n;
  }
  if (freeSlot) { freeSlot->used = true; freeSlot->id = id; freeSlot->mac = mac; }
  return freeSlot;
}

void webIngestRecord(const String& recordJson, const char* fromIp) {
  JsonDocument d;
  if (deserializeJson(d, recordJson)) return;
  String id  = d["id"]  | "";
  String mac = d["mac"] | "";
  lock();
  NodeState* n = upsert(id, mac);
  if (n) {
    n->room = String((const char*)(d["room"] | ""));
    if (fromIp) n->ip = fromIp;
    n->lastSeen = millis();
    n->last = recordJson;
  }
  unlock();
}

static String nodesJson() {
  JsonDocument doc;
  doc["role"]   = kutsuRoleStr(kutsuGetRole());
  doc["master"] = kutsuName();
  JsonArray arr = doc["nodes"].to<JsonArray>();
  lock();
  for (auto& n : s_nodes) {
    if (!n.used) continue;
    JsonObject o = arr.add<JsonObject>();
    o["id"] = n.id; o["mac"] = n.mac; o["room"] = n.room; o["ip"] = n.ip;
    o["age"] = (millis() - n.lastSeen) / 1000;
    if (n.last.length()) o["last"] = serialized(n.last);   // embed record verbatim
  }
  unlock();
  String s; serializeJson(doc, s); return s;
}

// ---- Route handlers --------------------------------------------------------
// send_P streams the ~15 KB PROGMEM page in 1460-byte chunks. Plain send() does
// ONE WiFiClient.write() of the whole body, which lwIP's TCP send buffer can't
// accept at this size -> the write short-returns and the browser gets an empty
// page. (The old ~5.5 KB page fit under the buffer, so send() happened to work.)
static void handleRoot()   { s_server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); }
static void handleNodes()  { s_server.send(200, "application/json", nodesJson()); }
static void handleHealth() {
  JsonDocument d; d["role"] = kutsuRoleStr(kutsuGetRole());
  d["name"] = kutsuName(); d["ip"] = kutsuMasterIp(); d["up"] = millis() / 1000;
  String s; serializeJson(d, s); s_server.send(200, "application/json", s);
}
static void handleIngest() {
  // Slaves POST one record as the request body ("plain" arg on WebServer).
  String body = s_server.hasArg("plain") ? s_server.arg("plain") : String();
  if (body.length()) webIngestRecord(body, s_server.client().remoteIP().toString().c_str());
  s_server.send(200, "application/json", "{\"ok\":1}");
}

static void webTask(void* pv) {
  s_server.begin();
  Serial.println("[web] master dashboard started on :" + String(WEB_PORT));
  while (s_running) {
    s_server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  s_server.stop();
  Serial.println("[web] master dashboard stopped (demoted to slave)");
  s_task = nullptr;
  vTaskDelete(nullptr);
}

void webMasterStart() {
  if (s_running) return;
  if (!s_mtx) s_mtx = xSemaphoreCreateMutex();

  s_server.on("/",                  HTTP_GET,  handleRoot);
  s_server.on("/api/nodes",         HTTP_GET,  handleNodes);
  s_server.on("/api/health",        HTTP_GET,  handleHealth);
  s_server.on("/api/ingest",        HTTP_POST, handleIngest);
  s_server.on("/api/nodes/register",HTTP_POST, handleIngest);   // records self-register too
  s_server.onNotFound([]() { s_server.send(404, "text/plain", "not found"); });

  s_running = true;
  // Own task so handleClient() is pumped promptly, independent of netTask's slow
  // heartbeat loop. 6 KB stack is plenty for the sync server + JSON building.
  xTaskCreate(webTask, "webTask", 6144, nullptr, 4, &s_task);
}

void webMasterStop() {
  if (!s_running) return;
  s_running = false;      // webTask sees this, stops the server, and self-deletes
}

bool webMasterRunning() { return s_running; }
