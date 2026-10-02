#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — WiFi provisioning + self-organizing node mesh
 * ---------------------------------------------------------------------------
 * Behaviour (req 4, per the approved plan):
 *   1. WiFi credentials arrive over BLE (netProvision) and persist in NVS.
 *   2. On association the node advertises mDNS `_kutsu._tcp` and browses for an
 *      existing MASTER.
 *   3. No master found  -> become MASTER: start the dashboard/ingest server
 *      (web_dashboard) and feed it this node's own samples.
 *      A master found    -> become SLAVE : push each sample to the master.
 *   4. Election race (two nodes claim master): the LOWER MAC keeps master, the
 *      other demotes to slave.
 *   5. Master lost (silent > MESH_MASTER_TIMEOUT): slaves re-run discovery so a
 *      new master is elected — no single hard point of failure.
 *
 * netTask() is the owning FreeRTOS task; the async web server runs its own.
 * =========================================================================== */

void netStart();     // create the net task (call once from setup, after identityInit)
void netTask(void* pv);

// Called from the BLE WiFi-provisioning characteristic. Stores creds + room in
// NVS and (re)connects. `room` may be empty to leave the current label.
void netProvision(const char* ssid, const char* pass, const char* room);

bool netWifiConnected();
