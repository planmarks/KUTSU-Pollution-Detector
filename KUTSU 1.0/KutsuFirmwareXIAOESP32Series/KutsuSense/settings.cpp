/* ===========================================================================
 * KUTSU Sense — device settings — implementation
 * ===========================================================================
 * Backed by the same "kutsu" NVS namespace the WiFi credentials use. Keys are
 * short to save NVS space: bz / led / disp / scan / mode. Cached in RAM so the
 * 1 Hz loop and the net task read them without touching flash every tick.
 * =========================================================================== */
#include "settings.h"
#include "config.h"
#include "identity.h"          // kutsuName / kutsuSetName (device name in JSON)
#include <Preferences.h>
#include <ArduinoJson.h>

static bool    s_buzzer  = true;
static bool    s_led     = true;
static bool    s_display = true;
static bool    s_scan    = true;
static uint8_t s_mode    = MODE_HANDHELD;
static uint8_t s_bzSig   = BZ_TRIPLE;         // buzzer signal pattern
static uint8_t s_ledWarn = LED_SLOW;          // LED behaviour at warning
static uint8_t s_ledAlert= LED_SOLID;         // LED behaviour at alert
static uint16_t s_sampleMs = SAMPLE_INTERVAL_MS; // logging cadence (#6)
static volatile bool s_bzPreview = false;     // request: play the signal once
static KutsuThresholds s_thr;                 // runtime thresholds (filled in init)

static void putBool(const char* key, bool v) {
  Preferences p; p.begin(NVS_NAMESPACE, false); p.putBool(key, v); p.end();
}
static void putU8(const char* key, uint8_t v) {
  Preferences p; p.begin(NVS_NAMESPACE, false); p.putUChar(key, v); p.end();
}
static void putU16(const char* key, uint16_t v) {
  Preferences p; p.begin(NVS_NAMESPACE, false); p.putUShort(key, v); p.end();
}
static uint16_t clampSampleMs(uint32_t ms) {
  if (ms < 1000)  return 1000;
  if (ms > 60000) return 60000;
  return (uint16_t)ms;
}

static void defaultThresholds(KutsuThresholds& t) {
  t.co2w=CO2_WARN;   t.co2h=CO2_HARM;
  t.eco2w=ECO2_WARN; t.eco2h=ECO2_HARM;
  t.tvocw=TVOC_WARN; t.tvoch=TVOC_HARM;
  t.vocw=VOC_WARN;   t.voch=VOC_HARM;
  t.noxw=NOX_WARN;   t.noxh=NOX_HARM;
  t.gasw=GAS_WARN;   t.gash=GAS_HARM;
  t.temp_wh=TEMP_WARN_HI; t.temp_hh=TEMP_HARM_HI; t.temp_hl=TEMP_HARM_LO;
  t.hum_hl=HUM_HARM_LO; t.hum_wl=HUM_WARN_LO; t.hum_wh=HUM_WARN_HI; t.hum_hh=HUM_HARM_HI;
}

void settingsInit() {
  Preferences p; p.begin(NVS_NAMESPACE, true);
  s_buzzer  = p.getBool ("bz",   true);
  s_led     = p.getBool ("led",  true);
  s_display = p.getBool ("disp", true);
  s_scan    = p.getBool ("scan", true);
  s_mode    = p.getUChar("mode", MODE_HANDHELD);
  s_bzSig   = p.getUChar("bzsig", BZ_TRIPLE);
  s_ledWarn = p.getUChar("ledw",  LED_SLOW);
  s_ledAlert= p.getUChar("leda",  LED_SOLID);
  s_sampleMs= clampSampleMs(p.getUShort("smpl", SAMPLE_INTERVAL_MS));
  defaultThresholds(s_thr);
  if (p.getBytesLength("thr") == sizeof(s_thr)) p.getBytes("thr", &s_thr, sizeof(s_thr));
  p.end();
  if (s_mode > MODE_STATIONARY)   s_mode    = MODE_HANDHELD;
  if (s_bzSig >= BZ_SIG_COUNT)     s_bzSig   = BZ_TRIPLE;
  if (s_ledWarn >= LED_MODE_COUNT) s_ledWarn = LED_SLOW;
  if (s_ledAlert >= LED_MODE_COUNT)s_ledAlert= LED_SOLID;
  Serial.printf("[settings] buzzer=%d led=%d display=%d scan=%d mode=%s sig=%d ledW=%d ledA=%d\n",
                s_buzzer, s_led, s_display, s_scan, settingsModeStr(), s_bzSig, s_ledWarn, s_ledAlert);
}

bool    settingsBuzzer()  { return s_buzzer; }
bool    settingsLed()     { return s_led; }
bool    settingsDisplay() { return s_display; }
bool    settingsScan()    { return s_scan; }
uint8_t settingsMode()    { return s_mode; }
const char* settingsModeStr() { return s_mode == MODE_STATIONARY ? "stationary" : "handheld"; }

uint16_t settingsSampleMs() { return s_sampleMs; }
void settingsSetSampleMs(uint16_t ms) {
  uint16_t v = clampSampleMs(ms);
  if (v != s_sampleMs) { s_sampleMs = v; putU16("smpl", v);
    Serial.printf("[settings] sample_ms -> %u\n", v); }
}

void settingsSetBuzzer(bool on)  { if (on != s_buzzer)  { s_buzzer  = on; putBool("bz",   on); } }
void settingsSetLed(bool on)     { if (on != s_led)     { s_led     = on; putBool("led",  on); } }
void settingsSetDisplay(bool on) { if (on != s_display) { s_display = on; putBool("disp", on); } }
void settingsSetScan(bool on)    { if (on != s_scan)    { s_scan    = on; putBool("scan", on); } }
void settingsSetMode(uint8_t m)  {
  if (m > MODE_STATIONARY) return;
  if (m != s_mode) { s_mode = m; putU8("mode", m);
    Serial.printf("[settings] mode -> %s\n", settingsModeStr()); }   // netTask re-evaluates on its next tick
}

// ---- Buzzer signal (4.2) ----------------------------------------------------
uint8_t settingsBuzzerSig() { return s_bzSig; }
void settingsSetBuzzerSig(uint8_t sig) {
  if (sig >= BZ_SIG_COUNT) return;
  s_bzSig = sig; putU8("bzsig", sig);
  s_bzPreview = true;                          // the loop plays the new signal once
}
bool settingsTakeBuzzerPreview() { if (!s_bzPreview) return false; s_bzPreview = false; return true; }

// ---- LED behaviour (4.3) ----------------------------------------------------
uint8_t settingsLedWarn()  { return s_ledWarn; }
uint8_t settingsLedAlert() { return s_ledAlert; }
void settingsSetLedWarn(uint8_t m)  { if (m < LED_MODE_COUNT && m != s_ledWarn)  { s_ledWarn  = m; putU8("ledw", m); } }
void settingsSetLedAlert(uint8_t m) { if (m < LED_MODE_COUNT && m != s_ledAlert) { s_ledAlert = m; putU8("leda", m); } }

// ---- Thresholds (4.1) -------------------------------------------------------
const KutsuThresholds& settingsThresholds() { return s_thr; }
static void saveThresholds() {
  Preferences p; p.begin(NVS_NAMESPACE, false); p.putBytes("thr", &s_thr, sizeof(s_thr)); p.end();
}
String settingsThresholdsJson() {
  JsonDocument d;
  d["co2"]["w"]=s_thr.co2w;   d["co2"]["h"]=s_thr.co2h;
  d["eco2"]["w"]=s_thr.eco2w; d["eco2"]["h"]=s_thr.eco2h;
  d["tvoc"]["w"]=s_thr.tvocw; d["tvoc"]["h"]=s_thr.tvoch;
  d["voc"]["w"]=s_thr.vocw;   d["voc"]["h"]=s_thr.voch;
  d["nox"]["w"]=s_thr.noxw;   d["nox"]["h"]=s_thr.noxh;
  d["gas"]["w"]=s_thr.gasw;   d["gas"]["h"]=s_thr.gash;
  d["temp"]["wh"]=s_thr.temp_wh; d["temp"]["hh"]=s_thr.temp_hh; d["temp"]["hl"]=s_thr.temp_hl;
  d["hum"]["hl"]=s_thr.hum_hl; d["hum"]["wl"]=s_thr.hum_wl; d["hum"]["wh"]=s_thr.hum_wh; d["hum"]["hh"]=s_thr.hum_hh;
  String s; serializeJson(d, s); return s;
}
bool settingsApplyThresholdsJson(const char* json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return false;
  KutsuThresholds t = s_thr;                    // start from current, patch present keys
  bool any = false;
  auto geti = [&](const char* m, const char* k, int& dst){ if (d[m][k].is<int>()) { dst = d[m][k]; any = true; } };
  auto getf = [&](const char* m, const char* k, float& dst){ if (d[m][k].is<float>()||d[m][k].is<int>()) { dst = d[m][k]; any = true; } };
  geti("co2","w",t.co2w);   geti("co2","h",t.co2h);
  geti("eco2","w",t.eco2w); geti("eco2","h",t.eco2h);
  geti("tvoc","w",t.tvocw); geti("tvoc","h",t.tvoch);
  geti("voc","w",t.vocw);   geti("voc","h",t.voch);
  geti("nox","w",t.noxw);   geti("nox","h",t.noxh);
  geti("gas","w",t.gasw);   geti("gas","h",t.gash);
  getf("temp","wh",t.temp_wh); getf("temp","hh",t.temp_hh); getf("temp","hl",t.temp_hl);
  getf("hum","hl",t.hum_hl); getf("hum","wl",t.hum_wl); getf("hum","wh",t.hum_wh); getf("hum","hh",t.hum_hh);
  if (d["reset"].as<bool>()) { defaultThresholds(t); any = true; }   // {"reset":true} restores defaults
  if (any) { s_thr = t; saveThresholds(); }
  return any;
}

String settingsJson() {
  JsonDocument d;
  d["name"]       = kutsuName();
  d["buzzer"]     = s_buzzer ? 1 : 0;
  d["led"]        = s_led ? 1 : 0;
  d["display"]    = s_display ? 1 : 0;
  d["scan"]       = s_scan ? 1 : 0;
  d["mode"]       = settingsModeStr();
  d["buzzer_sig"] = s_bzSig;
  d["led_warn"]   = s_ledWarn;
  d["led_alert"]  = s_ledAlert;
  d["sample_ms"]  = s_sampleMs;
  String s; serializeJson(d, s); return s;
}

// Accept a bool from true/false, 1/0, or "on"/"off".
static bool asBool(JsonVariant v, bool& out) {
  if (v.is<bool>())  { out = v.as<bool>(); return true; }
  if (v.is<int>())   { out = v.as<int>() != 0; return true; }
  if (v.is<const char*>()) {
    String s = v.as<const char*>(); s.toLowerCase();
    if (s == "on"  || s == "true"  || s == "1") { out = true;  return true; }
    if (s == "off" || s == "false" || s == "0") { out = false; return true; }
  }
  return false;
}

bool settingsApplyJson(const char* json) {
  JsonDocument d;
  if (deserializeJson(d, json)) return false;
  bool any = false, b;
  if (!d["buzzer"].isNull()  && asBool(d["buzzer"],  b)) { settingsSetBuzzer(b);  any = true; }
  if (!d["led"].isNull()     && asBool(d["led"],     b)) { settingsSetLed(b);     any = true; }
  if (!d["display"].isNull() && asBool(d["display"], b)) { settingsSetDisplay(b); any = true; }
  if (!d["scan"].isNull()    && asBool(d["scan"],    b)) { settingsSetScan(b);    any = true; }
  if (!d["mode"].isNull()) {
    JsonVariant mv = d["mode"];
    if (mv.is<const char*>()) {
      String s = mv.as<const char*>(); s.toLowerCase();
      if (s == "handheld")   { settingsSetMode(MODE_HANDHELD);   any = true; }
      if (s == "stationary") { settingsSetMode(MODE_STATIONARY); any = true; }
    } else if (mv.is<int>()) { settingsSetMode(mv.as<int>() ? MODE_STATIONARY : MODE_HANDHELD); any = true; }
  }
  if (d["name"].is<const char*>())     { kutsuSetName(d["name"].as<const char*>()); any = true; }  // "" reverts to creature
  if (d["buzzer_sig"].is<int>())       { settingsSetBuzzerSig((uint8_t)(int)d["buzzer_sig"]); any = true; }
  if (d["led_warn"].is<int>())         { settingsSetLedWarn((uint8_t)(int)d["led_warn"]);    any = true; }
  if (d["led_alert"].is<int>())        { settingsSetLedAlert((uint8_t)(int)d["led_alert"]);  any = true; }
  if (d["sample_ms"].is<int>())        { settingsSetSampleMs((uint16_t)(int)d["sample_ms"]); any = true; }
  return any;
}
