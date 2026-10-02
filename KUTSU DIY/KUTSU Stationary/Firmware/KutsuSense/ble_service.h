#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — BLE GATT layer (app link)
 * ---------------------------------------------------------------------------
 * Uses the ESP32 core's bundled BLE library (BLEDevice.h), which is NimBLE-backed
 * on the C5 — NOT the external NimBLE-Arduino (that one crashes on the C5). See
 * ble_service.cpp header for why. Advertises as kutsuName() ("Kutsu-A1B2C3"); the
 * BLE address (MAC) is inherently in the advertisement and also exposed via the
 * Device Information Service (req 2.3 / 2.4).
 *
 * One "Kutsu Environmental Service" (UUIDs in config.h):
 *   LIVE     notify        -> the current buildRecordJson() record, 1/sample
 *   TIMEGEO  write         -> {"epoch":..,"lat":..,"lon":..}  (req 2.2)
 *   WIFI     write         -> {"ssid":"..","password":"..","room":".."}
 *   STATUS   read/notify   -> {"name","mac","role","ip","synced","room"}
 *   HISTORY  read          -> tail of the JSONL log for on-device app storage
 *
 * bleInit() sets everything up + starts advertising; blePump() is called from
 * the BLE task and notifies LIVE once per new sample. Keeps advertising after
 * WiFi connects so the app can always reach the device.
 * =========================================================================== */

void bleStart();     // create the BLE task (call once from setup, after identityInit)
void bleTask(void* pv);

// Push a fresh STATUS notify (call when role/ip/room/sync state changes).
void bleNotifyStatus();
