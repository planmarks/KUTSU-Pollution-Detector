#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — device identity (MAC-derived, stable)
 * ---------------------------------------------------------------------------
 * Every unit needs a unique, human-readable name that survives a reflash and
 * needs no stored state. We derive it from the factory eFuse MAC (globally
 * unique, burned at manufacture), so two boards never collide. The mesh,
 * dashboard and JSON log all key on this name.
 *
 *   suffix   = last 3 bytes of the WiFi-STA base MAC, hex, UPPER  -> "A1B2C3"
 *   name     = "Kutsu-" + suffix                                  -> "Kutsu-A1B2C3"
 *   macStr   = full colon MAC                                     -> "AA:BB:CC:A1:B2:C3"
 *   hostname = "kutsu-" + suffix, lower (mDNS-safe)               -> "kutsu-a1b2c3"
 *
 * Call identityInit() once early in setup() (before BLE / WiFi / mDNS start).
 * All getters return pointers to cached, null-terminated static buffers.
 * =========================================================================== */

void        identityInit();
const char* kutsuSuffix();    // "A1B2C3"
const char* kutsuName();      // DISPLAY name: custom > creature (e.g. "Otter-C3") > "Kutsu-A1B2C3"
const char* kutsuMacStr();    // "AA:BB:CC:A1:B2:C3"
const char* kutsuHostname();  // "kutsu-a1b2c3" (MAC-based, stays stable + mDNS-safe)
const uint8_t* kutsuMacBytes(); // 6 raw bytes (for lower-MAC election tiebreak)

// ---- Device name (4.4 / 4.5) -----------------------------------------------
// At first boot a random CREATURE name is generated and stored (survives reflash,
// regenerated only on a full erase). The user may override it with a custom name.
// kutsuName() returns custom > creature > "Kutsu-<suffix>". The MAC stays the
// stable unique id (records carry both), and the mDNS hostname is unchanged.
void        kutsuSetName(const char* name);   // set custom name; "" reverts to the creature name
const char* kutsuCreatureName();              // the auto-generated creature name
