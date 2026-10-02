/* ===========================================================================
 * KUTSU Sense — WiFi provisioning + self-organizing node mesh
 * ===========================================================================
 * Built-in libs: WiFi, ESPmDNS, HTTPClient, Preferences. mDNS TXT read
 * (MDNS.txt) needs a recent esp32 core — the README already requires 3.3.x+.
 * =========================================================================== */
#include "mesh.h"
#include "identity.h"
#include "kutsu_link.h"
#include "web_dashboard.h"
#include "config.h"

#include <WiFi.h>
#include <ESPmDNS.h>
#include <HTTPClient.h>
#include <Preferences.h>

static Preferences   s_prefs;
static String        s_ssid, s_pass;
static volatile bool s_reprovision = false;   // set by netProvision (BLE task)
static bool          s_mdnsUp = false;
static uint32_t      s_lastMasterSeen = 0;     // slave: last good push to master

// ---- Credential persistence (NVS) -----------------------------------------
static void loadCreds() {
  s_prefs.begin(NVS_NAMESPACE, /*readonly=*/true);
  s_ssid = s_prefs.getString("ssid", "");
  s_pass = s_prefs.getString("pass", "");
  String room = s_prefs.getString("room", "");
  s_prefs.end();
  if (room.length()) kutsuSetRoom(room.c_str());
}

void netProvision(const char* ssid, const char* pass, const char* room) {
  s_prefs.begin(NVS_NAMESPACE, /*readonly=*/false);
  s_prefs.putString("ssid", ssid);
  s_prefs.putString("pass", pass);
  if (room && room[0]) s_prefs.putString("room", room);
  s_prefs.end();
  if (room && room[0]) kutsuSetRoom(room);
  s_reprovision = true;                        // netTask picks this up and reconnects
}

bool netWifiConnected() { return WiFi.status() == WL_CONNECTED; }

// ---- mDNS advertise (re-published whenever the role changes) --------------
static void publishMdns(KutsuRole role) {
  if (s_mdnsUp) { MDNS.end(); s_mdnsUp = false; }
  if (!MDNS.begin(kutsuHostname())) { Serial.println("[mesh] mDNS begin failed"); return; }
  MDNS.addService(MDNS_SERVICE, MDNS_PROTO, WEB_PORT);
  MDNS.addServiceTxt(MDNS_SERVICE, MDNS_PROTO, "name", kutsuName());
  MDNS.addServiceTxt(MDNS_SERVICE, MDNS_PROTO, "mac",  kutsuMacStr());
  MDNS.addServiceTxt(MDNS_SERVICE, MDNS_PROTO, "role", kutsuRoleStr(role));
  MDNS.addServiceTxt(MDNS_SERVICE, MDNS_PROTO, "room", kutsuRoom());
  s_mdnsUp = true;
  Serial.printf("[mesh] mDNS %s.local role=%s\n", kutsuHostname(), kutsuRoleStr(role));
}

// Browse `_kutsu._tcp`. Fills masterIp/masterMac with the first advertised
// master that ISN'T us. Returns true if such a master exists.
static bool findMaster(IPAddress& masterIp, String& masterMac) {
  int n = MDNS.queryService(MDNS_SERVICE, MDNS_PROTO);
  for (int i = 0; i < n; i++) {
    String mac  = MDNS.txt(i, "mac");
    String role = MDNS.txt(i, "role");
    if (mac == kutsuMacStr()) continue;         // that's our own record
    if (role == "master") { masterIp = MDNS.address(i); masterMac = mac; return true; }
  }
  return false;
}

// Is there another MASTER on the LAN whose MAC is LOWER than ours? (used by a
// sitting master to detect an election clash and gracefully step down.)
static bool lowerMasterExists(IPAddress& ip) {
  int n = MDNS.queryService(MDNS_SERVICE, MDNS_PROTO);
  for (int i = 0; i < n; i++) {
    String mac  = MDNS.txt(i, "mac");
    String role = MDNS.txt(i, "role");
    if (mac == kutsuMacStr() || role != "master") continue;
    if (mac < String(kutsuMacStr())) { ip = MDNS.address(i); return true; }   // hex compare == byte compare
  }
  return false;
}

static void becomeMaster() {
  kutsuSetRole(ROLE_MASTER);
  kutsuSetMasterIp(WiFi.localIP().toString().c_str());
  publishMdns(ROLE_MASTER);
  webMasterStart();
  Serial.printf("[mesh] -> MASTER, dashboard http://%s.local/\n", kutsuHostname());
}
static void becomeSlave(const IPAddress& masterIp) {
  if (webMasterRunning()) webMasterStop();
  kutsuSetRole(ROLE_SLAVE);
  kutsuSetMasterIp(masterIp.toString().c_str());
  publishMdns(ROLE_SLAVE);
  s_lastMasterSeen = millis();
  Serial.printf("[mesh] -> SLAVE, master %s\n", masterIp.toString().c_str());
}

static void runElection() {
  IPAddress masterIp; String masterMac;
  // Bring the mDNS responder up first (as "solo") so we can both answer queries
  // and browse — MDNS.queryService needs MDNS.begin() to have run.
  publishMdns(ROLE_SOLO);
  // Give existing/late-joining masters a moment to answer before we decide.
  delay(MESH_DISCOVER_MS);
  if (findMaster(masterIp, masterMac)) becomeSlave(masterIp);
  else                                 becomeMaster();
}

// Slave: POST the current record to the master's ingest endpoint.
static bool pushToMaster() {
  String url = String("http://") + kutsuMasterIp() + "/api/ingest";
  HTTPClient http; WiFiClient client;
  if (!http.begin(client, url)) return false;
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(kutsuCurrentRecordJson());
  http.end();
  return code > 0 && code < 400;
}

static bool connectWifi() {
  if (!s_ssid.length()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(kutsuHostname());
  WiFi.begin(s_ssid.c_str(), s_pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_CONNECT_MS) delay(200);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[mesh] WiFi up: %s\n", WiFi.localIP().toString().c_str());
    return true;
  }
  Serial.println("[mesh] WiFi connect timed out");
  return false;
}

void netTask(void* pv) {
  loadCreds();
  bool elected = false;

  for (;;) {
    // (Re)provisioned over BLE: reload creds and force a fresh connect+election.
    if (s_reprovision) {
      s_reprovision = false;
      loadCreds();
      WiFi.disconnect(true);
      elected = false;
      kutsuSetRole(ROLE_SOLO);
    }

    if (!s_ssid.length()) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }   // BLE-only until provisioned

    if (WiFi.status() != WL_CONNECTED) {
      elected = false;
      if (!connectWifi()) { vTaskDelay(pdMS_TO_TICKS(5000)); continue; }
    }

    if (!elected) { runElection(); elected = true; }

    // Steady-state tick per role.
    if (kutsuGetRole() == ROLE_MASTER) {
      webIngestRecord(kutsuCurrentRecordJson(), nullptr);   // feed own room into the dashboard
      IPAddress lower;
      if (lowerMasterExists(lower)) { becomeSlave(lower); } // election clash: step down
    } else if (kutsuGetRole() == ROLE_SLAVE) {
      if (pushToMaster()) {
        s_lastMasterSeen = millis();
      } else if (millis() - s_lastMasterSeen > MESH_MASTER_TIMEOUT) {
        Serial.println("[mesh] master unreachable; re-electing");
        elected = false;                                    // master gone -> re-discover/elect
      }
    }

    vTaskDelay(pdMS_TO_TICKS(MESH_HEARTBEAT_MS));
  }
}

void netStart() {
  xTaskCreate(netTask, "netTask", 8192, nullptr, 4, nullptr);
}
