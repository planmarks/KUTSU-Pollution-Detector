#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — core <-> connectivity link
 * ---------------------------------------------------------------------------
 * The sensing/UI core (KutsuSense.ino) and the connectivity modules (mesh =
 * WiFi + hub client, ble_provisioning) run in different FreeRTOS tasks and must not touch each
 * other's internals directly. This header is the *only* seam between them:
 *
 *   - the core OWNS the sensor state and the data contract (buildRecordJson)
 *     and exposes a thread-safe snapshot + time/role setters here;
 *   - the connectivity modules READ the latest record, SET the clock once NTP
 *     answers, and SET the node role once the mesh election resolves.
 *
 * All functions below are implemented in KutsuSense.ino and are safe to call
 * from any task (they take the shared-state mutex internally).
 * =========================================================================== */

// solo = stand-alone (no hub reached); slave = feeding a KUTSU Hub. MASTER is
// kept only for record compatibility with KUTSU Stationary (v2) nodes.
enum KutsuRole { ROLE_SOLO = 0, ROLE_MASTER, ROLE_SLAVE };
const char* kutsuRoleStr(KutsuRole r);      // "solo" | "master" | "slave"

// ---- Data contract (single source of truth lives in buildRecordJson) -------
// Returns the JSON record for the most recent completed sample. Safe from any task.
String   kutsuCurrentRecordJson();

// Sequence counter that bumps once per completed sample.
uint32_t kutsuSampleSeq();

// ---- Time -------------------------------------------------------------------
// The net task supplies wall-clock epoch once NTP answers. The core derives
// bootEpoch = epoch - uptimeS, stamps every record from then on, and appends a
// one-off {"ev":"timesync",...} anchor line to the log.
void     kutsuApplyTimeSync(uint32_t epoch, const char* src = "ntp");  // src: "ntp" | "app" | "usb"
bool     kutsuTimeSynced();
uint32_t kutsuEpochNow();       // bootEpoch + uptime, or 0 if unsynced

// ---- Geolocation (device has no GPS) ---------------------------------------
// Contributed by a connected phone / desktop app / USB host. Stored and stamped
// onto every later record as lat/lon/loc_ts. Empty (null in the record) until a
// fix is supplied. loc_ts = epoch of the fix, or 0 if time isn't synced yet.
void     kutsuApplyGeo(double lat, double lon);
bool     kutsuHaveGeo();
double   kutsuLat();
double   kutsuLon();
uint32_t kutsuLocTs();

// ---- Room label (per-node, from config.h / serial provisioning) ------------
void        kutsuSetRoom(const char* room);
String      kutsuRoom();        // "" until set

// ---- Role (set by the hub client in mesh.cpp) ------------------------------
void        kutsuSetRole(KutsuRole r);
KutsuRole   kutsuGetRole();
void        kutsuSetMasterIp(const char* ip);   // slave: "ip:port" of the hub it feeds
String      kutsuMasterIp();

// ---- Stored log read-back (#3: SD dump over BLE) ---------------------------
// Expose the on-storage JSON-Lines log (SD if present, else internal flash) for
// streaming to the app. Both take the log mutex internally, so they interleave
// safely with ongoing logging. kutsuLogByteSize() is the current total size;
// kutsuLogReadChunk() copies up to `cap` bytes starting at `offset` into `out`
// and returns the count (0 at/after EOF or on error).
bool   kutsuLogEnabled();      // a log target (SD or flash) is active
size_t kutsuLogByteSize();
size_t kutsuLogReadChunk(size_t offset, char* out, size_t cap);

// ---- Radio arbitration -----------------------------------------------------
// Every XIAO has a single 2.4 GHz radio. The core's passive WiFi scans and the
// net task's association attempts must not overlap (a scan in flight makes
// WiFi.begin() fail). Whoever holds this lock may start a scan / connect.
bool kutsuRadioTake(uint32_t waitMs);
void kutsuRadioGive();
