#pragma once
#include <Arduino.h>

/* ===========================================================================
 * KUTSU Sense — device settings (user-toggleable, persisted in NVS)
 * ---------------------------------------------------------------------------
 * Settings a connected client (desktop app / mobile app / local webapp) may
 * change over BLE, USB serial, or the LAN API — see PROVISIONING.md / SETTINGS.
 * They persist across reboots in the "kutsu" NVS namespace and default to the
 * first-launch behaviour: everything on, MODE = handheld.
 *
 *   buzzer  : audible alarm on/off
 *   led     : warning LED on/off
 *   display : OLED on/off (device keeps sensing/logging/serving when off)
 *   scan    : passive WiFi + BLE scanning on/off (advertising stays on)
 *   mode    : HANDHELD (stand-alone / local) vs STATIONARY (joins the node mesh)
 *
 * MODE is distinct from the network ROLE (solo/master/slave): mode is the
 * user's choice; role is what the mesh election makes of a STATIONARY unit.
 * A HANDHELD unit never joins the mesh — on WiFi it only serves its own data
 * locally. See mesh.cpp.
 * =========================================================================== */

enum KutsuMode { MODE_HANDHELD = 0, MODE_STATIONARY = 1 };

void settingsInit();        // load from NVS (defaults if unset). Call once in setup().

bool    settingsBuzzer();
bool    settingsLed();
bool    settingsDisplay();
bool    settingsScan();
uint8_t settingsMode();     // KutsuMode
const char* settingsModeStr();   // "handheld" | "stationary"

// Setters persist to NVS immediately. MODE changes are picked up by netTask
// (mesh.cpp) on its next heartbeat — no reboot needed.
void settingsSetBuzzer(bool on);
void settingsSetLed(bool on);
void settingsSetDisplay(bool on);
void settingsSetScan(bool on);
void settingsSetMode(uint8_t mode);

// ---- Sensor refresh / logging rate (#6) -------------------------------------
// How often a record is logged to SD/flash (and emitted to the mesh/app history),
// in milliseconds. The sensor loop still runs at the native 1 Hz (the SGP41
// VOC/NOx gas-index algorithm requires it); this only decimates logging, so a
// higher value means fewer records and longer battery life. Clamped 1000-60000.
uint16_t settingsSampleMs();
void     settingsSetSampleMs(uint16_t ms);

// ---- Buzzer signal (4.2) ----------------------------------------------------
// Which alarm pattern the buzzer plays. Changing it requests a one-off PREVIEW
// that the main loop plays so the user hears the new signal.
enum KutsuBuzzerSig { BZ_TRIPLE = 0, BZ_SINGLE, BZ_LONG, BZ_RAPID, BZ_SOS, BZ_SIG_COUNT };
uint8_t settingsBuzzerSig();
void    settingsSetBuzzerSig(uint8_t sig);   // persists + requests a preview
bool    settingsTakeBuzzerPreview();         // loop: true once after a change, then clears

// ---- LED behaviour per severity (4.3) ---------------------------------------
// How the warning LED behaves for a WARNING (sev 1) and an ALERT (sev 2).
enum KutsuLedMode { LED_OFF = 0, LED_SLOW, LED_FAST, LED_SOLID, LED_SOS, LED_MODE_COUNT };
uint8_t settingsLedWarn();                   // behaviour at warning level
uint8_t settingsLedAlert();                  // behaviour at alert level
void    settingsSetLedWarn(uint8_t mode);
void    settingsSetLedAlert(uint8_t mode);

// ---- Warning thresholds (4.1) -----------------------------------------------
// Runtime copy of config.h's defaults; editable by a connected client and stored
// in NVS. `evaluate()` reads these instead of the macros.
struct KutsuThresholds {
  int   co2w, co2h, eco2w, eco2h, tvocw, tvoch, vocw, voch, noxw, noxh, gasw, gash;
  float temp_wh, temp_hh, temp_hl;             // warn-hi, harm-hi, harm-lo
  float hum_hl, hum_wl, hum_wh, hum_hh;        // harm-lo, warn-lo, warn-hi, harm-hi
};
const KutsuThresholds& settingsThresholds();
String settingsThresholdsJson();
bool   settingsApplyThresholdsJson(const char* json);   // apply any subset; persists

// JSON view for BLE/serial/web. Includes name (from identity) + the 4.x config.
// {"name":"Otter-C3","buzzer":1,"led":1,"display":1,"scan":1,"mode":"handheld",
//  "buzzer_sig":0,"led_warn":1,"led_alert":3}
String settingsJson();

// Apply any subset of those fields from a JSON object (incl. "name", "buzzer_sig",
// "led_warn", "led_alert"). `mode` accepts "handheld"/"stationary" or 0/1.
// Returns true if at least one field was valid.
bool settingsApplyJson(const char* json);
