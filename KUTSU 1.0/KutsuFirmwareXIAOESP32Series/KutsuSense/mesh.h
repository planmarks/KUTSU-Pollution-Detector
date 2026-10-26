#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — WiFi link + KUTSU Hub client
 * ---------------------------------------------------------------------------
 * Behaviour:
 *   1. Credentials come from config.h (WIFI_SSID/WIFI_PASSWORD) or, taking
 *      precedence, from NVS (USB serial `ssid`/`pass`/`connect`, or the BLE
 *      WIFI characteristic).
 *   2. HANDHELD mode never joins WiFi. No credentials, or the network can't be
 *      joined -> the unit stays a stand-alone handheld (display + alarm +
 *      SD/flash log + passive scans) and retries every WIFI_RETRY_MS.
 *   3. STATIONARY mode, once associated: NTP time, then find the KUTSU Hub
 *      (the collector/dashboard app running on a Raspberry Pi or PC on the
 *      LAN — see ../../KutsuHub):
 *        - HUB_ADDRESS / `hub <host[:port]>` if set, else
 *        - UDP discovery: broadcast a probe on HUB_UDP_PORT, the hub answers
 *          (it also beacons periodically).
 *   4. With a hub: every HUB_PUSH_MS POST the current reading to /api/live and
 *      stream any NEW lines of the SD/flash JSONL log to /api/ingest?src=log.
 *      The byte offset already delivered is remembered, so anything logged
 *      while the hub or WiFi was down is sent when it comes back (from the
 *      moment the hub was first reached in this boot).
 *   5. Hub silent > HUB_TIMEOUT_MS -> rediscover (auto mode). Link lost ->
 *      handheld until the network returns.
 *
 * The device no longer serves a web page itself; the hub is the dashboard.
 * netTask() is the owning FreeRTOS task.
 * =========================================================================== */

enum NetState {
  NET_NO_CREDS = 0,   // no SSID configured (or handheld mode) -> stand-alone
  NET_CONNECTING,     // association attempt in progress
  NET_ONLINE,         // joined (role SLAVE once a hub accepts our data)
  NET_OFFLINE         // last attempt failed / link lost -> stand-alone, will retry
};

void netStart();     // create the net task (call once from setup, after identityInit)

// Store credentials (+ optional room) in NVS and reconnect. An empty ssid
// disables WiFi (handheld only) until new credentials are given.
void netProvision(const char* ssid, const char* pass, const char* room);

// Hub address: "host[:port]" pins it; "" (or "auto") = UDP auto-discovery. Persisted.
void   netSetHub(const char* addr);
String netHubSetting();     // "" = auto
String netHub();            // "ip:port" of the hub currently in use, "" if none

// ---- Status for the UI / serial console (safe from any task) ---------------
NetState netState();
const char* netStateStr(NetState s);
bool     netWifiConnected();
String   netSsid();         // configured SSID ("" if none)
String   netIp();           // own IP while online, else ""
int      netRssi();         // dBm while online, else 0
uint32_t netRetryInS();     // seconds until the next attempt (NET_OFFLINE), 0 if none scheduled
uint32_t netBacklogBytes(); // log bytes not yet delivered to the hub
