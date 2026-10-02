#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — device identity (MAC-derived, stable)
 * ---------------------------------------------------------------------------
 * Every unit needs a unique, human-readable name that survives a reflash and
 * needs no stored state. We derive it from the factory eFuse MAC (globally
 * unique, burned at manufacture), so two boards never collide and the same
 * value is what BLE advertises anyway (req 2.3 / 2.4).
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
const char* kutsuName();      // "Kutsu-A1B2C3"
const char* kutsuMacStr();    // "AA:BB:CC:A1:B2:C3"
const char* kutsuHostname();  // "kutsu-a1b2c3"
const uint8_t* kutsuMacBytes(); // 6 raw bytes (for lower-MAC election tiebreak)
