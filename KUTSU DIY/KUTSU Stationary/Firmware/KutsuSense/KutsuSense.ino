/* ===========================================================================
 * KUTSU Sense — firmware for the Espressif ESP32-C5-DevKitC-1 (Arduino framework)
 * ---------------------------------------------------------------------------
 * Environmental monitor: reads air quality (eCO2, TVOC), temperature, humidity,
 * and MQ-2 gas/smoke; shows them on a 128x64 I2C OLED; charts the last hour;
 * raises a pulsing full-screen alarm + 3-beep buzzer on harmful readings; drives
 * three status LEDs; logs one JSON record per sample to MicroSD (flash fallback)
 * for a Wi-Fi dashboard / BLE app; and writes a fixed NFC health record.
 *
 * PORT: XIAO ESP32-C5 build (../KutsuXIAOESP32C5) moved to the ESP32-C5-DevKitC-1.
 * Key differences vs. the XIAO build:
 *   1. Pins are RAW GPIO numbers (the DevKitC has no D0..D10 board macros).
 *   2. Three status LEDs (see updateLeds()):
 *        GOOD  -> green solid | WARN -> green+yellow solid |
 *        ALERT -> red blink   | ERROR(sensor init fail) -> yellow blink.
 *   3. MQ-2 gas/smoke read on GPIO3 (ADC1_CH2, via a ÷2 divider) as a relative
 *      raw-ADC level with a presence check (see readGas()/config.h).
 *   4. WiFi/BLE passive-scan screens removed (radios stay off).
 *   5. Logging is now JSON Lines (one JSON object per sample) — see logRow()/
 *      buildRecordJson() and DATA_CONTRACT.md — so the record maps 1:1 to what a
 *      web dashboard (Wi-Fi) or mobile app (BLE) will later consume.
 *
 * SEE ALSO: config.h (tunables/pins), WIRING.md (wiring + LEDs + MQ-2),
 *           DATA_CONTRACT.md (JSON log schema), README.md (build/flash/bring-up).
 *
 * HARDWARE (pins in config.h). I2C @ GPIO6/GPIO8 needs external 4.7k pull-ups to
 * 3V3. Devices: OLED 0x3C, HTU21D 0x40, CCS811 0x5A (WAK->GND!), NTAG 0x55.
 * MicroSD on SPI (GPIO23/24/9/10). Buzzer=GPIO4. Button=GPIO1. MQ-2 AO=GPIO3
 * (VCC=5V, ÷2 divider). LEDs: green=GPIO5, yellow=GPIO26, red=GPIO0.
 *
 * LIBRARIES: Adafruit SSD1306, GFX, HTU21DF, CCS811 (+ BusIO).
 * SD, LittleFS, Wire, SPI are built into the ESP32 core.
 *
 * DATA FLOW each 1 s tick: readSensors() -> historyAdd() -> evaluate()
 *   -> set LED mode -> (alarm pulse 5s on / 1s gap + 3-beep-once) or
 *   renderScreen() -> logRow(). updateBuzzer(), handleButtons() and updateLeds()
 *   (which drives the blink) run every loop iteration.
 * =========================================================================== */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_CCS811.h>

#include "config.h"
#include "identity.h"       // MAC-derived name/suffix/mac (req 2.3/2.4)
#include "kutsu_link.h"     // core <-> BLE/mesh/web seam (this file implements it)
#include "ble_service.h"    // BLE GATT app link
#include "mesh.h"           // WiFi provisioning + node mesh/election

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
Adafruit_HTU21DF htu;
Adafruit_CCS811  ccs;
SPIClass         spiSD(FSPI);      // if the C5 core doesn't define FSPI, use SPI or HSPI

bool okHtu = false, okCcs = false;
bool useSD = false, useFlash = false;

int32_t lastEco2 = -1, lastTvoc = -1;

struct Reading {
  bool  htuOk, ccsOk, gasOk;
  int32_t eco2, tvoc;
  float tempC, hum;
  int   gasRaw;                    // MQ-2 raw ADC 0-4095 (relative level)
};

struct Eval {
  int sevTvoc, sevEco2, sevTemp, sevHum, sevGas;
  bool harmful;
  const char* msg;
  String detail;
};



const int    NM = 4;               // charted metrics (gas is live-only, not charted)
const char*  CH_LABEL[NM] = {"eCO2", "TVOC", "Temp", "Hum"};
const float  CH_MIN[NM]   = {400, 0, 0, 0};
const float  CH_MAX[NM]   = {2500, 3000, 70, 100};
float         chSeries[NM][CHART_BINS];
double        chAccSum[NM];
int           chAccN[NM];
unsigned long chMinuteStart = 0;

// Screens the button cycles through, in order.
enum { VIEW_LIVE = 0, VIEW_STATS, NUM_VIEWS };

// LED status modes (rendered every loop by updateLeds()).
enum LedMode { LED_GOOD = 0, LED_WARN, LED_WARN_MANY, LED_ALERT, LED_ERROR };
LedMode       ledMode = LED_GOOD;

int           view = VIEW_LIVE;
bool          alarmShowing = false;
unsigned long alarmPhaseEnd = 0;
bool          warningLatched = false;
bool          alarmAcked = false;   // set when the button dismisses an active alarm
unsigned long nextTick = 0, startMs = 0;
bool          btnPrev = HIGH;
Reading       lastR;
Eval          lastE;

// ===========================================================================
// Shared state for the BLE / mesh / web tasks — implements kutsu_link.h
// ---------------------------------------------------------------------------
// The sensing loop (this file's loop(), its own FreeRTOS task) owns lastR/lastE
// and the log; the BLE and net tasks reach them ONLY through the functions
// below, which take the two mutexes. g_snap is the latest completed sample.
// ===========================================================================
static SemaphoreHandle_t g_stateMtx = nullptr;    // guards g_snap / room / master ip
static SemaphoreHandle_t g_logMtx   = nullptr;    // serialises SD/flash access

struct Snapshot { Reading r; Eval e; unsigned long uptimeS; bool fault; bool valid; };
static Snapshot           g_snap = {};
static volatile uint32_t  g_sampleSeq = 0;

static volatile uint32_t  g_bootEpoch = 0;        // 0 = time not synced yet
static double             g_lat = 0, g_lon = 0;
static uint32_t           g_locTs = 0;
static bool               g_haveGeo = false;
static char               g_room[24] = "";
static volatile KutsuRole g_role = ROLE_SOLO;
static char               g_masterIp[20] = "";

static inline void L(SemaphoreHandle_t m) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
static inline void U(SemaphoreHandle_t m) { if (m) xSemaphoreGive(m); }

const char* kutsuRoleStr(KutsuRole r) {
  return r == ROLE_MASTER ? "master" : (r == ROLE_SLAVE ? "slave" : "solo");
}
uint32_t kutsuSampleSeq()  { return g_sampleSeq; }
bool     kutsuTimeSynced() { return g_bootEpoch != 0; }
uint32_t kutsuBootEpoch()  { return g_bootEpoch; }
uint32_t kutsuEpochNow()   {
  if (!g_bootEpoch) return 0;
  return g_bootEpoch + (uint32_t)((millis() - startMs) / 1000);
}
void        kutsuSetRoom(const char* room) { L(g_stateMtx); strlcpy(g_room, room ? room : "", sizeof g_room); U(g_stateMtx); }
const char* kutsuRoom()                    { return g_room; }
void        kutsuSetRole(KutsuRole r)      { g_role = r; }
KutsuRole   kutsuGetRole()                 { return g_role; }
void        kutsuSetMasterIp(const char* ip){ L(g_stateMtx); strlcpy(g_masterIp, ip ? ip : "", sizeof g_masterIp); U(g_stateMtx); }
const char* kutsuMasterIp()                { return g_masterIp; }

struct Seg { bool on; uint16_t ms; };
// Alert tone: 3 short beeps (on / gap / on / gap / on).
const Seg BEEP[] = {
  {true,120},{false,120},{true,120},{false,120},{true,120},
};
const int BEEP_N = sizeof(BEEP) / sizeof(BEEP[0]);
int           beepIdx = -1;
unsigned long beepSegEnd = 0;

void startBeep() {
  beepIdx = 0;
  digitalWrite(PIN_BUZZER, BEEP[0].on ? HIGH : LOW);
  beepSegEnd = millis() + BEEP[0].ms;
}
void stopBeep() {                  // silence immediately (used by the dismiss button)
  beepIdx = -1;
  digitalWrite(PIN_BUZZER, LOW);
}
void updateBuzzer() {
  if (beepIdx < 0) return;
  if (millis() >= beepSegEnd) {
    beepIdx++;
    if (beepIdx >= BEEP_N) { digitalWrite(PIN_BUZZER, LOW); beepIdx = -1; return; }
    digitalWrite(PIN_BUZZER, BEEP[beepIdx].on ? HIGH : LOW);
    beepSegEnd = millis() + BEEP[beepIdx].ms;
  }
}

// ---------------------------------------------------------------------------
// Status LEDs — visual indicator for users with no display
// ---------------------------------------------------------------------------
//   GOOD      : green solid            (all readings good)
//   WARN      : green + yellow solid    (a few metrics at the "!" warning level)
//   WARN_MANY : yellow solid only       (>WARN_MANY_COUNT metrics warned — green off,
//                                        so the observer reads "conditions suboptimal")
//   ALERT     : red blinking            (any metric at the alert level)
//   ERROR     : yellow blinking         (a required sensor failed to initialise)
// ledMode is set once per tick; updateLeds() is called every loop iteration so
// the blink is smooth and non-blocking.
void ledsInit() {
  pinMode(PIN_LED_GREEN,  OUTPUT);
  pinMode(PIN_LED_YELLOW, OUTPUT);
  pinMode(PIN_LED_RED,    OUTPUT);
  digitalWrite(PIN_LED_GREEN,  LOW);
  digitalWrite(PIN_LED_YELLOW, LOW);
  digitalWrite(PIN_LED_RED,    LOW);
}
void ledsSelfTest() {              // brief power-on check: each LED blinks in turn
  const int pins[3] = {PIN_LED_GREEN, PIN_LED_YELLOW, PIN_LED_RED};
  for (int i = 0; i < 3; i++) {
    digitalWrite(pins[i], HIGH); delay(180);
    digitalWrite(pins[i], LOW);
  }
}
void updateLeds() {
  bool blink = ((millis() / LED_BLINK_MS) % 2) == 0;   // shared blink phase
  bool g = false, y = false, r = false;
  switch (ledMode) {
    case LED_GOOD:      g = true;             break;
    case LED_WARN:      g = true; y = true;   break;
    case LED_WARN_MANY: y = true;             break;   // >3 metrics warned: green off, yellow only
    case LED_ALERT:     r = blink;            break;   // red blinking, others off
    case LED_ERROR:     y = blink;            break;   // yellow blinking, others off
  }
  digitalWrite(PIN_LED_GREEN,  g ? HIGH : LOW);
  digitalWrite(PIN_LED_YELLOW, y ? HIGH : LOW);
  digitalWrite(PIN_LED_RED,    r ? HIGH : LOW);
}

// MQ-2 gas: average GAS_SAMPLES raw ADC reads on GPIO3 (via the ÷2 divider).
int readGas() {
  long acc = 0;
  for (int i = 0; i < GAS_SAMPLES; i++) acc += analogRead(PIN_MQ2_AO);
  return (int)(acc / GAS_SAMPLES);
}

Reading readSensors() {
  Reading r; r.htuOk = r.ccsOk = r.gasOk = false;
  r.eco2 = r.tvoc = -1; r.tempC = r.hum = NAN; r.gasRaw = 0;

  if (okHtu) {
    float t = htu.readTemperature(), h = htu.readHumidity();
    if (!isnan(t) && !isnan(h)) { r.tempC = t; r.hum = h; r.htuOk = true; }
  }
  if (okCcs && ccs.available()) {
    if (!ccs.readData()) { lastEco2 = ccs.geteCO2(); lastTvoc = ccs.getTVOC(); }
  }
  r.eco2 = lastEco2; r.tvoc = lastTvoc; r.ccsOk = (lastEco2 >= 0);

  r.gasRaw = readGas();
  r.gasOk  = (r.gasRaw >= GAS_PRESENT_MIN);   // below floor => sensor absent/unpowered

  return r;
}

void historyInit() {
  for (int i = 0; i < NM; i++) {
    for (int b = 0; b < CHART_BINS; b++) chSeries[i][b] = NAN;
    chAccSum[i] = 0; chAccN[i] = 0;
  }
}
float metricValue(const Reading& r, int i) {
  switch (i) {
    case 0:  return r.ccsOk ? (float)r.eco2   : NAN;
    case 1:  return r.ccsOk ? (float)r.tvoc   : NAN;
    case 2:  return r.htuOk ? r.tempC         : NAN;
    default: return r.htuOk ? r.hum           : NAN;
  }
}
void historyAdd(const Reading& r, unsigned long now) {
  if (chMinuteStart == 0) chMinuteStart = now;
  for (int i = 0; i < NM; i++) {
    float v = metricValue(r, i);
    if (!isnan(v)) { chAccSum[i] += v; chAccN[i]++; }
  }
  if (now - chMinuteStart >= CHART_BIN_MS) {
    for (int i = 0; i < NM; i++) {
      float avg = chAccN[i] ? (float)(chAccSum[i] / chAccN[i]) : NAN;
      for (int b = 0; b < CHART_BINS - 1; b++) chSeries[i][b] = chSeries[i][b + 1];
      chSeries[i][CHART_BINS - 1] = avg;
      chAccSum[i] = 0; chAccN[i] = 0;
    }
    chMinuteStart = now;
  }
}

Eval evaluate(const Reading& r) {
  Eval e;
  e.sevTvoc = e.sevEco2 = e.sevTemp = e.sevHum = e.sevGas = 0;
  e.harmful = false; e.msg = ""; e.detail = "";

  if (r.ccsOk) {
    if (r.tvoc >= TVOC_HARM) e.sevTvoc = 2; else if (r.tvoc >= TVOC_WARN) e.sevTvoc = 1;
    if (r.eco2 >= ECO2_HARM) e.sevEco2 = 2; else if (r.eco2 >= ECO2_WARN) e.sevEco2 = 1;
  }
  if (r.htuOk) {
    if (r.tempC > TEMP_HARM_HI || r.tempC < TEMP_HARM_LO) e.sevTemp = 2;
    else if (r.tempC > TEMP_WARN_HI) e.sevTemp = 1;
    if (r.hum < HUM_HARM_LO || r.hum > HUM_HARM_HI) e.sevHum = 2;
    else if (r.hum < HUM_WARN_LO || r.hum > HUM_WARN_HI) e.sevHum = 1;
  }
  if (r.gasOk) {
    if (r.gasRaw >= GAS_HARM) e.sevGas = 2; else if (r.gasRaw >= GAS_WARN) e.sevGas = 1;
  }

  e.harmful = (e.sevTvoc == 2 || e.sevEco2 == 2 ||
               e.sevTemp == 2 || e.sevHum == 2 || e.sevGas == 2);
  if      (e.sevEco2 == 2) { e.msg = "HIGH CO2";        e.detail = "eCO2: " + String(r.eco2)    + "ppm"; }
  else if (e.sevTvoc == 2) { e.msg = "HIGH TVOC";       e.detail = "TVOC: " + String(r.tvoc)    + "ppb"; }
  else if (e.sevGas  == 2) { e.msg = "HIGH GAS";        e.detail = "Gas: "  + String(r.gasRaw)  + " (raw)"; }
  else if (e.sevTemp == 2) { e.msg = "UNSAFE TEMP";     e.detail = "Temp: " + String(r.tempC, 1) + "C"; }
  else if (e.sevHum  == 2) { e.msg = "UNSAFE HUMIDITY"; e.detail = "Hum: "  + String(r.hum, 1)   + "%"; }
  return e;
}

// Overall (worst) severity across all metrics -> feeds the LED mode + JSON log.
int overallSeverity(const Eval& e) {
  int s = e.sevEco2;
  if (e.sevTvoc > s) s = e.sevTvoc;
  if (e.sevTemp > s) s = e.sevTemp;
  if (e.sevHum  > s) s = e.sevHum;
  if (e.sevGas  > s) s = e.sevGas;
  return s;
}

void centerText(const char* s, int y, int size) {
  display.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((OLED_W - (int)w) / 2, y);
  display.print(s);
}

void showWelcome() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawRoundRect(0, 0, OLED_W, OLED_H, 6, SSD1306_WHITE);
  centerText("KUTSU", 7, 2);
  centerText("6th Sense", 28, 1);
  display.drawFastHLine(24, 40, OLED_W - 48, SSD1306_WHITE);
  centerText("By Papathought", 49, 1);
  display.display();
}

// Second boot screen: a live log of module-init results, drawn line by line.
int bootY = 0;
void bootBegin() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
  display.setCursor(0, 0); display.print("Init modules:");
  display.display();
  bootY = 8;                        // 8px pitch fits header + module lines in 64px
}
void bootLine(const char* label, const char* status) {
  display.setCursor(0, bootY);  display.print(label);
  display.setCursor(72, bootY); display.print(status);
  display.display();
  bootY += 8;
  delay(300);                     // let each result register on screen
}

const int X_LABEL = 0, X_VALUE = 40, X_UNIT = 80, X_FLAG = 112;
void liveRow(int y, const char* label, bool ok, const String& val, const char* unit, int sev) {
  display.setCursor(X_LABEL, y); display.print(label);
  display.setCursor(X_VALUE, y); display.print(ok ? val : String("--"));
  display.setCursor(X_UNIT, y);  display.print(unit);
  if (sev) { display.setCursor(X_FLAG, y); display.print(sev == 2 ? "!!" : "!"); }
}

void showLive() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
  // 5 rows at 12px pitch fit the 64px height (eCO2/TVOC/Temp/Hum/Gas).
  liveRow(2,  "eCO2", lastR.ccsOk, String(lastR.eco2),     "ppm",     lastE.sevEco2);
  liveRow(14, "TVOC", lastR.ccsOk, String(lastR.tvoc),     "ppb",     lastE.sevTvoc);
  liveRow(26, "Temp", lastR.htuOk, String(lastR.tempC, 1), "\xF8""C", lastE.sevTemp);
  liveRow(38, "Hum",  lastR.htuOk, String(lastR.hum, 1),   "%RH",     lastE.sevHum);
  liveRow(50, "Gas",  lastR.gasOk, String(lastR.gasRaw),   "adc",     lastE.sevGas);
  display.display();
}

const int CHARTS_PER_PAGE = NM;            // all charted metrics on a single stats page

void showCharts(int page) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  int band = OLED_H / CHARTS_PER_PAGE, barW = 2, x0 = OLED_W - CHART_BINS * barW;
  for (int k = 0; k < CHARTS_PER_PAGE; k++) {
    int i = page * CHARTS_PER_PAGE + k;
    if (i >= NM) break;
    int top = k * band, plotTop = top + 9, plotH = band - 10;
    float cur = NAN;
    for (int b = 0; b < CHART_BINS; b++) if (!isnan(chSeries[i][b])) cur = chSeries[i][b];
    display.setCursor(0, top);
    display.print(CH_LABEL[i]); display.print(" ");
    if (isnan(cur)) display.print("--"); else display.print((int)roundf(cur));
    for (int b = 0; b < CHART_BINS; b++) {
      float v = chSeries[i][b]; if (isnan(v)) continue;
      float frac = (v - CH_MIN[i]) / (CH_MAX[i] - CH_MIN[i]);
      if (frac < 0) frac = 0; if (frac > 1) frac = 1;
      int h = (int)(frac * plotH), x = x0 + b * barW;
      if (h > 0) display.fillRect(x, plotTop + plotH - h, barW, h, SSD1306_WHITE);
    }
  }
  display.display();
}

void showAlarm(const Eval& e) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  centerText("!! WARNING !!", 6, 1);
  centerText(e.msg, 26, 1);
  display.setCursor(4, 44); display.setTextSize(1); display.print(e.detail);
  display.display();
}

void renderScreen() {
  switch (view) {
    case VIEW_LIVE:  showLive();    break;
    default:         showCharts(0); break;
  }
}

// ---------------------------------------------------------------------------
// Logging — one JSON object per line (JSON Lines / NDJSON)
// ---------------------------------------------------------------------------
// buildRecordJson() is the single source of truth for the data contract: the
// SAME string written to SD here is what a future Wi-Fi (web dashboard) or BLE
// (mobile app) layer will transmit. Missing/absent sensors log JSON null so a
// consumer can tell "no data" from a real zero. Schema in DATA_CONTRACT.md.
static String jInt(bool ok, long v)              { return ok ? String(v) : String("null"); }
static String jFloat(bool ok, float v, int dp)   { return ok ? String(v, dp) : String("null"); }

String buildRecordJson(const Reading& r, const Eval& e, unsigned long uptimeS, bool fault) {
  uint32_t ep = kutsuEpochNow();                     // 0 until a phone syncs the clock
  String s = "{";
  s += "\"v\":" + String(SCHEMA_VERSION);
  s += ",\"id\":\"" + String(kutsuName()) + "\"";    // MAC-derived, e.g. "Kutsu-A1B2C3"
  s += ",\"mac\":\"" + String(kutsuMacStr()) + "\"";
  s += ",\"role\":\"" + String(kutsuRoleStr(kutsuGetRole())) + "\"";
  s += ",\"room\":\"" + String(kutsuRoom()) + "\"";
  s += ",\"up\":" + String(uptimeS);                 // seconds since boot
  s += ",\"ts\":" + (ep ? String(ep) : String("null")); // epoch s (null until synced)
  s += ",\"sev\":" + String(overallSeverity(e));     // 0 ok / 1 warn / 2 alert
  s += ",\"alarm\":" + String(e.harmful ? 1 : 0);
  s += ",\"fault\":" + String(fault ? 1 : 0);        // a sensor failed to init
  s += ",\"eco2\":" + jInt(r.ccsOk, r.eco2);         // ppm
  s += ",\"tvoc\":" + jInt(r.ccsOk, r.tvoc);         // ppb
  s += ",\"temp\":" + jFloat(r.htuOk, r.tempC, 1);   // deg C
  s += ",\"hum\":"  + jFloat(r.htuOk, r.hum, 1);     // %RH
  s += ",\"gas\":"  + jInt(r.gasOk, r.gasRaw);       // MQ-2 raw ADC 0-4095
  s += ",\"s_eco2\":" + String(e.sevEco2);
  s += ",\"s_tvoc\":" + String(e.sevTvoc);
  s += ",\"s_temp\":" + String(e.sevTemp);
  s += ",\"s_hum\":"  + String(e.sevHum);
  s += ",\"s_gas\":"  + String(e.sevGas);
  s += ",\"lat\":"    + (g_haveGeo ? String(g_lat, 6) : String("null"));
  s += ",\"lon\":"    + (g_haveGeo ? String(g_lon, 6) : String("null"));
  s += ",\"loc_ts\":" + (g_haveGeo ? String(g_locTs) : String("null"));
  s += "}";
  return s;
}

void enforceFlashCap() {
  File f = LittleFS.open(FLASH_LOG_PATH, FILE_READ);
  if (!f) return;
  size_t sz = f.size(); f.close();
  if (sz <= FLASH_LOG_MAX) return;
  LittleFS.remove(FLASH_LOG_PATH);                   // JSONL has no header to rewrite
  Serial.println("[log] flash cap hit, log reset");
}
// All log writes funnel through here so the sensor loop and the BLE task (which
// appends the timesync anchor) never corrupt the file. Holds g_logMtx.
static void appendLogLine(const String& line) {
  L(g_logMtx);
  if (useSD) {
    File f = SD.open(SD_LOG_PATH, FILE_APPEND); if (f) { f.println(line); f.close(); }
  } else if (useFlash) {
    File f = LittleFS.open(FLASH_LOG_PATH, FILE_APPEND); if (f) { f.println(line); f.close(); }
    enforceFlashCap();
  }
  U(g_logMtx);
}
void logRow(const Reading& r, unsigned long uptimeS, bool fault) {
  appendLogLine(buildRecordJson(r, lastE, uptimeS, fault));
}

// ---- kutsu_link.h: snapshot / log tail / time+geo sync --------------------
// Build the JSON for the most recent completed sample (called by BLE + mesh).
String kutsuCurrentRecordJson() {
  Snapshot s; L(g_stateMtx); s = g_snap; U(g_stateMtx);
  if (!s.valid) return String("{}");
  return buildRecordJson(s.r, s.e, s.uptimeS, s.fault);
}
// Tail of the active log, aligned to whole lines (BLE history characteristic).
String kutsuTailLog(size_t maxBytes) {
  String out; L(g_logMtx);
  File f;
  if      (useSD)    f = SD.open(SD_LOG_PATH, FILE_READ);
  else if (useFlash) f = LittleFS.open(FLASH_LOG_PATH, FILE_READ);
  if (f) {
    size_t sz = f.size(), start = sz > maxBytes ? sz - maxBytes : 0;
    f.seek(start);
    if (start > 0) while (f.available()) { if (f.read() == '\n') break; }  // drop partial line
    while (f.available() && out.length() < maxBytes) out += (char)f.read();
    f.close();
  }
  U(g_logMtx);
  return out;
}
// A phone supplied wall-clock (+ optional location). Anchor uptime->epoch and
// append a one-off timesync record so all prior lines become resolvable (2.2).
void kutsuApplyTimeSync(uint32_t epoch, bool haveGeo, double lat, double lon) {
  uint32_t up = (uint32_t)((millis() - startMs) / 1000);
  g_bootEpoch = (epoch > up) ? epoch - up : epoch;
  if (haveGeo) { g_lat = lat; g_lon = lon; g_haveGeo = true; g_locTs = epoch; }
  String a = "{\"ev\":\"timesync\",\"id\":\"" + String(kutsuName()) +
             "\",\"up\":" + String(up) + ",\"ts\":" + String(epoch);
  if (haveGeo) a += ",\"lat\":" + String(lat, 6) + ",\"lon\":" + String(lon, 6);
  a += "}";
  appendLogLine(a);
}

void nfcWriteBlock(uint8_t block, const uint8_t* data16) {
  Wire.beginTransmission(ADDR_NTAG);
  Wire.write(block);
  for (int i = 0; i < 16; i++) Wire.write(data16[i]);
  Wire.endTransmission();
}
void nfcProvision() {
  String text = String("Name: ") + NFC_FULL_NAME + "\nDOB: " + NFC_DOB + "\nBlood: " + NFC_BLOOD;
  uint8_t payloadLen = 1 + 2 + text.length();
  uint8_t buf[256]; int n = 0;
  buf[n++] = 0x03;
  buf[n++] = 4 + payloadLen;
  buf[n++] = 0xD1; buf[n++] = 0x01; buf[n++] = payloadLen; buf[n++] = 'T';
  buf[n++] = 0x02; buf[n++] = 'e'; buf[n++] = 'n';
  for (size_t i = 0; i < text.length(); i++) buf[n++] = text[i];
  buf[n++] = 0xFE;
  while (n % 16) buf[n++] = 0x00;
  for (int i = 0; i < n; i += 16) nfcWriteBlock(1 + i / 16, &buf[i]);
  Serial.println("[nfc] health record written (verify NT3H2111 block map on hardware)");
}


void handleButtons() {
  bool b1 = digitalRead(PIN_BUTTON);
  if (btnPrev == HIGH && b1 == LOW) {
    if (lastE.harmful && !alarmAcked) {
      // Press during an active alarm -> dismiss: silence the buzzer, drop the
      // warning screen, and return to live readings.
      alarmAcked   = true;
      alarmShowing = false;
      alarmPhaseEnd = 0;
      stopBeep();
      view = 0;
      renderScreen();
    } else {                                   // otherwise cycle: live -> stats -> live
      view = (view + 1) % NUM_VIEWS;
      if (!alarmShowing) renderScreen();
    }
  }
  btnPrev = b1;
}

// How many metrics are currently flagged (warning or worse).
int countWarnings(const Eval& e) {
  int n = 0;
  if (e.sevEco2 >= 1) n++;
  if (e.sevTvoc >= 1) n++;
  if (e.sevTemp >= 1) n++;
  if (e.sevHum  >= 1) n++;
  if (e.sevGas  >= 1) n++;
  return n;
}

// Decide the LED mode from the current fault/severity. Priority: an active ALERT
// from a working sensor outranks an init ERROR (a failed sensor can't raise sev2
// anyway), then WARN, then GOOD. Within WARN, if MORE than WARN_MANY_COUNT metrics
// are flagged at once, drop green and hold yellow only (conditions suboptimal).
void setLedMode(const Eval& e, bool fault) {
  int sev = overallSeverity(e);
  if      (sev == 2) ledMode = LED_ALERT;
  else if (fault)    ledMode = LED_ERROR;
  else if (sev == 1) ledMode = (countWarnings(e) > WARN_MANY_COUNT) ? LED_WARN_MANY : LED_WARN;
  else               ledMode = LED_GOOD;
}

void setup() {
  Serial.begin(115200);
  identityInit();                    // MAC-derived name/mac (before BLE/WiFi)
  Serial.printf("[main] %s  mac=%s\n", kutsuName(), kutsuMacStr());
  g_stateMtx = xSemaphoreCreateMutex();
  g_logMtx   = xSemaphoreCreateMutex();
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  analogReadResolution(12);          // MQ-2 AO on GPIO3: 0-4095
  ledsInit();
  ledsSelfTest();              // blink G/Y/R once so a display-less user sees the LEDs work

  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(I2C_HZ);

  Serial.printf("[main] I2C on SDA=GPIO%d SCL=GPIO%d, scanning...\n", PIN_SDA, PIN_SCL);
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("  found 0x%02X\n", a); }
  }

  bool okOled = display.begin(SSD1306_SWITCHCAPVCC, ADDR_SSD1306);
  if (!okOled) Serial.println("[display] init failed");
  display.cp437(true);
  showWelcome();
  delay(1800);                 // hold the welcome screen

  bootBegin();                 // 2nd boot screen: live module-init log
  bootLine("OLED",   okOled ? "OK" : "FAIL");
  bootLine("LEDs",   "OK");    // (self-test already ran above)

  okHtu = htu.begin();               if (!okHtu) Serial.println("[sensors] HTU21D init failed");
  bootLine("HTU21D", okHtu ? "OK" : "FAIL");
  okCcs = ccs.begin(ADDR_CCS811);    if (!okCcs) Serial.println("[sensors] CCS811 init failed (check WAK->GND)");
  bootLine("CCS811", okCcs ? "OK" : "FAIL");

  {                            // one-shot MQ-2 presence probe for the boot log
    int g = readGas();
    bootLine("MQ-2", g >= GAS_PRESENT_MIN ? "OK" : "absent");
    Serial.printf("[sensors] MQ-2 raw=%d (%s)\n", g, g >= GAS_PRESENT_MIN ? "present" : "absent/unpowered");
  }

  spiSD.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  useSD = SD.begin(PIN_SD_CS, spiSD, SD_HZ);
  if (useSD) {
    Serial.println("[log] SD card mounted");
  }
  else {
    Serial.println("[log] no SD card; trying flash");
    useFlash = LittleFS.begin(true);
    if (useFlash) Serial.println("[log] logging to flash");
    else          Serial.println("[log] flash init failed; not logging");
  }
  bootLine("Logging", useSD ? "SD" : (useFlash ? "Flash" : "none"));

  nfcProvision();
  bootLine("NFC",    "sent");

  bootLine("ID",     kutsuSuffix());   // MAC-derived unit id (Kutsu-<suffix>)

  historyInit();

  delay(1800);                 // hold the boot screen
  startMs = millis();
  nextTick = millis();
  lastR = readSensors();
  lastE = evaluate(lastR);
  setLedMode(lastE, !okHtu || !okCcs);

  // Bring up the radios as their own FreeRTOS tasks so BLE/WiFi/web never stall
  // the 1 Hz sensing loop or the alarm. BLE advertises immediately; the net task
  // stays idle until the app provisions WiFi over BLE, then joins/forms the mesh.
  bleStart();
  netStart();
}

void loop() {
  unsigned long now = millis();
  updateBuzzer();
  handleButtons();
  updateLeds();                     // drives the blink; runs every iteration

  if (now >= nextTick) {
    nextTick += SAMPLE_INTERVAL_MS;
    unsigned long uptimeS = (now - startMs) / 1000;
    bool fault = (!okHtu || !okCcs); // a required I2C sensor failed to initialise

    lastR = readSensors();
    historyAdd(lastR, now);
    lastE = evaluate(lastR);
    setLedMode(lastE, fault);        // GOOD/WARN/ALERT/ERROR for the status LEDs

    // Publish the completed sample for the BLE/mesh/web tasks (one bump/tick).
    L(g_stateMtx);
    g_snap.r = lastR; g_snap.e = lastE; g_snap.uptimeS = uptimeS;
    g_snap.fault = fault; g_snap.valid = true;
    U(g_stateMtx);
    g_sampleSeq++;

    // Harmful -> beep once (latched) and pulse the alarm screen: 5 s on, 1 s gap
    // showing live readings, repeat while the condition persists. A button press
    // acknowledges it (alarmAcked) -> silent + normal screens until it clears.
    if (lastE.harmful) {
      if (!alarmAcked) {
        if (!warningLatched) { warningLatched = true; startBeep(); }   // once per warning
        if (!alarmShowing && now >= alarmPhaseEnd) {
          alarmShowing = true; alarmPhaseEnd = now + ALARM_SHOW_MS; showAlarm(lastE);
        } else if (alarmShowing && now >= alarmPhaseEnd) {
          alarmShowing = false; alarmPhaseEnd = now + ALARM_GAP_MS; renderScreen();
        } else if (!alarmShowing) {
          renderScreen();
        }
      } else {
        renderScreen();          // acknowledged: stay silent on the normal screen
      }
    } else {
      alarmShowing = false; alarmPhaseEnd = 0; warningLatched = false; alarmAcked = false;
      renderScreen();
    }
    logRow(lastR, uptimeS, fault);
  }
  delay(5);
}
