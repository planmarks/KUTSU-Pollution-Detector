#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — core <-> connectivity link
 * ---------------------------------------------------------------------------
 * The sensing/UI core (KutsuSense.ino) and the connectivity modules
 * (ble_service, mesh, web_dashboard) run in different FreeRTOS tasks and must
 * not touch each other's internals directly. This header is the *only* seam
 * between them:
 *
 *   - the core OWNS the sensor state and the data contract (buildRecordJson)
 *     and exposes a thread-safe snapshot + time/geo/role setters here;
 *   - the connectivity modules READ the latest record, WRITE time/geo when a
 *     phone syncs, and SET the node role once the mesh election resolves.
 *
 * All functions below are implemented in KutsuSense.ino and are safe to call
 * from any task (they take the shared-state mutex internally).
 * =========================================================================== */

enum KutsuRole { ROLE_SOLO = 0, ROLE_MASTER, ROLE_SLAVE };
const char* kutsuRoleStr(KutsuRole r);      // "solo" | "master" | "slave"

// ---- Data contract (single source of truth lives in buildRecordJson) -------
// Returns the JSON record for the most recent completed sample, already
// carrying v2 fields (id/mac/role/room/ts/lat/lon). Safe from any task.
String   kutsuCurrentRecordJson();

// Sequence counter that bumps once per completed sample, so a consumer (BLE
// notify / mesh push) can send exactly one record per tick without polling.
uint32_t kutsuSampleSeq();

// ---- Time & geolocation sync (req 2.2) ------------------------------------
// A connected phone supplies wall-clock epoch (+ optional lat/lon). The core
// derives bootEpoch = epoch - uptimeS and stamps every record from then on,
// and appends a one-off {"ev":"timesync",...} anchor line to the log.
void     kutsuApplyTimeSync(uint32_t epoch, bool haveGeo, double lat, double lon);
bool     kutsuTimeSynced();
uint32_t kutsuBootEpoch();      // 0 until synced
uint32_t kutsuEpochNow();       // bootEpoch + uptime, or 0 if unsynced

// ---- Room label (per-node, set during provisioning) -----------------------
void        kutsuSetRoom(const char* room);
const char* kutsuRoom();        // "" until set

// ---- Role (set by the mesh election) --------------------------------------
void      kutsuSetRole(KutsuRole r);
KutsuRole kutsuGetRole();
void      kutsuSetMasterIp(const char* ip);   // slave: master it feeds; master: own ip
const char* kutsuMasterIp();

// ---- Log access (for the BLE history characteristic) ----------------------
// Returns up to maxBytes from the tail of the active JSONL log (SD or flash),
// aligned to whole lines. Empty if not logging. Safe from any task.
String    kutsuTailLog(size_t maxBytes);
