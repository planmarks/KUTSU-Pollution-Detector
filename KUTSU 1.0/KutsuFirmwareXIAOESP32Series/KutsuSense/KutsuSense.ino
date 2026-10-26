/* ===========================================================================
 * KUTSU Sense — KUTSU 1.0 firmware for the Seeed XIAO ESP32 series (Arduino)
 * ONE sketch for XIAO ESP32-C3 / C5 / C6 / S3 on the Kutsu 1 board
 * ---------------------------------------------------------------------------
 * Environmental monitor. At boot it scans the I2C bus and brings up whatever
 * it finds — the on-board Sensirion stack (SCD41 CO2, SGP41 VOC+NOx, SHT40
 * temp/RH), the original DIY breakouts (CCS811 eCO2/TVOC, HTU21D temp/RH),
 * either display (the on-board 0.96" OLED panel or a plug-in SSD1306 module),
 * plus an optional MQ-2 on D0. Only detected sensors are shown, charted and
 * logged. A pulsing full-screen alarm + 3-beep buzzer + warning LED fire on
 * harmful readings; records are logged as JSON Lines to MicroSD (internal
 * flash fallback). Nearby WiFi APs and BLE devices are passively scanned on
 * two screens.
 *
 * KUTSU HUB: in STATIONARY mode the unit joins WiFi, finds the KUTSU Hub (a
 * collector + dashboard app on a Raspberry Pi / PC on the LAN, ../../KutsuHub)
 * and streams its readings + log to it (mesh.cpp). With no network or no hub
 * it keeps working stand-alone; the net task retries in the background and
 * never blocks the sensing loop.
 *
 * SEE ALSO:
 *   - config.h ........... ALL tunables: pins, addresses, thresholds, WiFi
 *   - README.md .......... board package + libraries + wiring + bring-up
 *   - FIRMWARE.md ........ internals / architecture
 *   - DATA_CONTRACT.md ... JSON record schema (log + hub)
 *
 * LIBRARIES: Adafruit SSD1306 + GFX (+ BusIO); Adafruit HTU21DF; Adafruit
 * CCS811; Sensirion I2C SCD4x, SHT4x, SGP41 + Gas Index Algorithm (+ Core);
 * ArduinoJson 7. WiFi/WiFiUdp/HTTPClient/BLE/SD/LittleFS/Wire/SPI/
 * Preferences are built into the ESP32 core (3.x). BLE scanning uses the
 * core's BLE library (NOT NimBLE-Arduino, which crashes on the C5).
 *
 * FILE MAP (top -> bottom):
 *   board selection .......... per-chip name / quirks (#if CONFIG_IDF_TARGET_*)
 *   metric table ............. MD[]: key/label/unit/chart range per metric
 *   globals/objects .......... peripherals, detection results, last-good values
 *   link state ............... kutsu_link.h implementation (snapshot, time, role)
 *   buzzer / LED ............. non-blocking warning outputs
 *   readSensors() ............ T/RH -> SCD41 -> SGP41(comp) -> CCS811(comp) -> MQ-2
 *   history* / evaluate() .... 60 one-minute bins / thresholds -> severity
 *   show*() .................. OLED screens: boot, live, stats, net, devices, dashboard, alarm
 *   WiFi/BLE scans ........... async AP scan + BLE callback table + SD scan logs
 *   logging .................. buildRecordJson + SD / rotating flash log
 *   console .................. USB serial commands (WiFi credentials, status)
 *   setup()/loop() ........... detect + init, then a 1 Hz tick
 * =========================================================================== */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <math.h>
#include <WiFi.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_system.h>

// On every supported XIAO, D6/D7 (the two buttons) are UART0 TX/RX. With "USB
// CDC On Boot" disabled, Serial and the panic console run on those pins, so
// logs vanish and the D6 button fights the TX line. Serial must use USB.
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || (ARDUINO_USB_CDC_ON_BOOT == 0)
#error "Set Tools -> USB CDC On Boot -> Enabled (XIAO D6/D7 are UART0 TX/RX = the buttons)"
#endif

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_HTU21DF.h>
#include <Adafruit_CCS811.h>
#include <SensirionI2cScd4x.h>   // new-gen driver (v1.x): note the lowercase "I2c"
#include <SensirionI2cSht4x.h>   // new-gen driver (v1.x): note the lowercase "I2c"
#include <SensirionI2CSgp41.h>   // classic driver: keeps the uppercase "I2C"
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

#include "config.h"
#include "identity.h"       // MAC-derived name/suffix/mac
#include "kutsu_link.h"     // core <-> mesh/web seam (this file implements it)
#include "mesh.h"           // WiFi link + KUTSU Hub client
#include "ble_provisioning.h" // BLE GATT: set up WiFi/room + status from the app
#include "settings.h"       // user toggles (buzzer/LED/display/scan/mode), NVS-backed

// ---------------------------------------------------------------------------
// Board selection — everything chip-specific lives here
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32C3)
  #define BOARD_NAME  "XIAO ESP32-C3"
  #define BOARD_SHORT "C3"
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
  #define BOARD_NAME  "XIAO ESP32-C5"
  #define BOARD_SHORT "C5"
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
  #define BOARD_NAME  "XIAO ESP32-C6"
  #define BOARD_SHORT "C6"
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
  #define BOARD_NAME  "XIAO ESP32-S3"
  #define BOARD_SHORT "S3"
#else
  #error "Unsupported board: select XIAO_ESP32C3, XIAO_ESP32C5, XIAO_ESP32C6 or XIAO_ESP32S3"
#endif

// ---------------------------------------------------------------------------
// Metric table — one row per quantity any supported sensor can produce
// ---------------------------------------------------------------------------
enum Metric { M_CO2 = 0, M_ECO2, M_TVOC, M_VOC, M_NOX, M_TEMP, M_HUM, M_GAS, NM };

struct MetricDef {
  const char* key;      // JSON key (DATA_CONTRACT.md)
  const char* label;    // OLED label (<= 4 chars)
  const char* unit;     // OLED unit (cp437)
  uint8_t     dp;       // decimals shown/logged
  float       chMin, chMax;   // stats-chart range
  const char* alarmMsg;
};
const MetricDef MD[NM] = {
  {"co2",  "CO2",  "ppm",      0, 400, 2500, "HIGH CO2"},
  {"eco2", "eCO2", "ppm",      0, 400, 2500, "HIGH eCO2"},
  {"tvoc", "TVOC", "ppb",      0, 0,   3000, "HIGH TVOC"},
  {"voc",  "VOC",  "idx",      0, 0,   500,  "HIGH VOC"},
  {"nox",  "NOx",  "idx",      0, 0,   500,  "HIGH NOx"},
  {"temp", "Temp", "\xF8""C",  1, 0,   70,   "UNSAFE TEMP"},
  {"hum",  "Hum",  "%RH",      1, 0,   100,  "UNSAFE HUMIDITY"},
  {"gas",  "Gas",  "adc",      0, 0,   4095, "HIGH GAS"},
};
// Which sev-2 metric names the alarm screen when several are harmful at once.
const Metric ALARM_PRIORITY[NM] = {M_CO2, M_ECO2, M_VOC, M_NOX, M_TVOC, M_GAS, M_TEMP, M_HUM};

// One on/off segment of a buzzer/LED pattern. Defined up here (before the first
// function) so the Arduino auto-prototype generator sees the type.
struct Seg { bool on; uint16_t ms; };

// ---------------------------------------------------------------------------
// Peripherals + detection results
// ---------------------------------------------------------------------------
Adafruit_SSD1306     display(OLED_W, OLED_H, &Wire, -1);   // panel/module: no reset line
SensirionI2cScd4x    scd4x;
SensirionI2cSht4x    sht4x;
SensirionI2CSgp41    sgp41;
VOCGasIndexAlgorithm vocAlgo;   // default sampling interval = 1 s
NOxGasIndexAlgorithm noxAlgo;
Adafruit_HTU21DF     htu;
Adafruit_CCS811      ccs;
SPIClass             spiSD(FSPI);

bool    i2cFound[128];                                // boot scan result
bool    okOled = false;
bool    okScd = false, okSgp = false, okSht = false, okHtu = false, okCcs = false;
bool    fitted[NM];                                   // metric has a working source
bool    g_fault = false;                              // a detected sensor failed to init
bool    useSD = false, useFlash = false;

int32_t  lastCo2 = -1, lastVoc = -1, lastNox = -1, lastEco2 = -1, lastTvoc = -1;
float    lastScdT = NAN, lastScdH = NAN;              // SCD41's own T/RH (fallback source)
uint16_t sgpCondLeft = SGP41_CONDITION_S;             // SGP41 conditioning countdown (1 Hz)
uint8_t  sgpFail = 0;                                  // consecutive SGP41 I2C read failures
uint32_t ccsEnvNext = 0;                              // next CCS811 T/RH compensation push

struct Reading { bool ok[NM]; float v[NM]; };
struct Eval    { uint8_t sev[NM]; bool harmful; int alarmMetric; };

float         chSeries[NM][CHART_BINS];
double        chAccSum[NM];
int           chAccN[NM];
unsigned long chMinuteStart = 0;

// Screens the button cycles through, in order.
enum { VIEW_LIVE = 0, VIEW_STATS, VIEW_NET, VIEW_BLE, VIEW_DASH, NUM_VIEWS };

int           view = VIEW_LIVE;
bool          alarmShowing = false;
unsigned long alarmPhaseEnd = 0;
bool          warningLatched = false;
bool          alarmAcked = false;   // set when the button dismisses an active alarm
unsigned long nextTick = 0, startMs = 0;
bool          btnPrev = HIGH;
Reading       lastR;
Eval          lastE;
uint32_t      sampleCount = 0;

uint8_t       animPhase = 0;        // drives the "..." scan animation + LED warn-blink (1 Hz)
unsigned long noticeEnd = 0;        // "dashboard online" banner
String        noticeL1, noticeL2;
NetState      prevNet = NET_NO_CREDS;

// ---- WiFi (async background scan) ----
struct ApInfo  { String ssid; int rssi; uint16_t freq; };
ApInfo        apList[NET_SHOW];
int           apShown = 0, apTotal = 0;
bool          wifiScanning = false;
unsigned long lastWifiKick = 0, lastNetLog = 0, lastLogMs = 0;

// ---- BLE (background scan -> aged table, filled from the BLE host task) ----
struct BleEntry { char mac[18]; char name[20]; int rssi; unsigned long lastMs; bool used; };
BleEntry          bleTab[BLE_TABLE_MAX];
SemaphoreHandle_t bleLock;           // guards bleTab (callback task vs. loop)
BLEScan*          bleScan = nullptr;
const uint32_t    BLE_SCAN_CYCLE_S = 10;   // restart (and clear the core's result list) this often

struct DevInfo { String name; int rssi; };
DevInfo       devList[BLE_SHOW];     // loop-owned display snapshot
int           devShown = 0, devTotal = 0;
unsigned long lastDevLog = 0;

// ===========================================================================
// Shared state for the mesh / web tasks — implements kutsu_link.h
// ---------------------------------------------------------------------------
// The sensing loop owns lastR/lastE and the log; the net and web tasks reach
// them ONLY through the functions below, which take the mutexes.
// ===========================================================================
static SemaphoreHandle_t g_stateMtx = nullptr;    // guards g_snap / room / master ip
static SemaphoreHandle_t g_logMtx   = nullptr;    // serialises SD/flash access
static SemaphoreHandle_t g_radioMtx = nullptr;    // passive scans vs. association

struct Snapshot { Reading r; Eval e; bool fitted[NM]; unsigned long uptimeS; bool valid; };
static Snapshot           g_snap = {};
static volatile uint32_t  g_sampleSeq = 0;
static volatile uint32_t  g_bootEpoch = 0;        // 0 = time not synced yet
static volatile bool      g_haveGeo = false;      // a geolocation fix has been supplied
static volatile double    g_lat = 0, g_lon = 0;   // last fix (WGS-84 degrees)
static volatile uint32_t  g_locTs = 0;            // epoch of the fix, 0 if time unsynced
static String             g_room;
static volatile KutsuRole g_role = ROLE_SOLO;
static String             g_masterIp;

static inline void L(SemaphoreHandle_t m) { if (m) xSemaphoreTake(m, portMAX_DELAY); }
static inline void U(SemaphoreHandle_t m) { if (m) xSemaphoreGive(m); }

const char* kutsuRoleStr(KutsuRole r) {
  return r == ROLE_MASTER ? "master" : (r == ROLE_SLAVE ? "slave" : "solo");
}
uint32_t  kutsuSampleSeq()  { return g_sampleSeq; }
bool      kutsuTimeSynced() { return g_bootEpoch != 0; }
uint32_t  kutsuEpochNow()   {
  if (!g_bootEpoch) return 0;
  return g_bootEpoch + (uint32_t)((millis() - startMs) / 1000);
}
void      kutsuSetRoom(const char* room)      { L(g_stateMtx); g_room = room ? room : ""; U(g_stateMtx); }
String    kutsuRoom()                         { L(g_stateMtx); String s = g_room; U(g_stateMtx); return s; }
void      kutsuSetRole(KutsuRole r)           { g_role = r; }
KutsuRole kutsuGetRole()                      { return g_role; }
void      kutsuSetMasterIp(const char* ip)    { L(g_stateMtx); g_masterIp = ip ? ip : ""; U(g_stateMtx); }
String    kutsuMasterIp()                     { L(g_stateMtx); String s = g_masterIp; U(g_stateMtx); return s; }
bool      kutsuRadioTake(uint32_t waitMs)     { return xSemaphoreTake(g_radioMtx, pdMS_TO_TICKS(waitMs)) == pdTRUE; }
void      kutsuRadioGive()                    { xSemaphoreGive(g_radioMtx); }

// ---------------------------------------------------------------------------
// Warning outputs: buzzer (3-beep-once) + LED (severity indicator)
// ---------------------------------------------------------------------------
// Selectable buzzer signals (4.2). `.` short, `-` long, SOS = ...---...
static const Seg P_TRIPLE[] = {{1,120},{0,120},{1,120},{0,120},{1,120}};
static const Seg P_SINGLE[] = {{1,200}};
static const Seg P_LONG[]   = {{1,800}};
static const Seg P_RAPID[]  = {{1,60},{0,60},{1,60},{0,60},{1,60},{0,60},{1,60},{0,60},{1,60}};
static const Seg P_SOS[]    = {{1,120},{0,120},{1,120},{0,120},{1,120},{0,300},   // S
                               {1,360},{0,120},{1,360},{0,120},{1,360},{0,300},   // O
                               {1,120},{0,120},{1,120},{0,120},{1,120}};          // S
struct SegSeq { const Seg* seg; int n; };
#define SEQ(a) { a, (int)(sizeof(a)/sizeof(a[0])) }
static const SegSeq BZ_TABLE[BZ_SIG_COUNT] = {   // indexed by KutsuBuzzerSig
  SEQ(P_TRIPLE), SEQ(P_SINGLE), SEQ(P_LONG), SEQ(P_RAPID), SEQ(P_SOS),
};
const Seg*    curBeep   = P_TRIPLE;  // pattern currently playing
int           curBeepN  = (int)(sizeof(P_TRIPLE)/sizeof(P_TRIPLE[0]));
int           beepIdx = -1;
unsigned long beepSegEnd = 0;

// On-board BZ1 is a magnetic transducer -> square-wave drive; an active
// buzzer (DIY) also works with BUZZER_TONE_HZ = 0 (plain DC level).
void buzzerSet(bool on) {
#if BUZZER_TONE_HZ > 0
  ledcWriteTone(PIN_BUZZER, on ? BUZZER_TONE_HZ : 0);
#else
  digitalWrite(PIN_BUZZER, on ? HIGH : LOW);
#endif
}
void startBeep() {
  const SegSeq& p = BZ_TABLE[settingsBuzzerSig() < BZ_SIG_COUNT ? settingsBuzzerSig() : 0];
  curBeep = p.seg; curBeepN = p.n;
  beepIdx = 0;
  buzzerSet(curBeep[0].on);
  beepSegEnd = millis() + curBeep[0].ms;
}
void stopBeep() {                  // silence immediately (used by the dismiss button)
  beepIdx = -1;
  buzzerSet(false);
}
void updateBuzzer() {
  // A buzzer-signal change requests a one-off preview so the user hears the new signal.
  if (settingsTakeBuzzerPreview() && settingsBuzzer()) startBeep();
  if (!settingsBuzzer()) { if (beepIdx >= 0) { buzzerSet(false); beepIdx = -1; } return; }  // buzzer disabled
  if (beepIdx < 0) return;
  if (millis() >= beepSegEnd) {
    beepIdx++;
    if (beepIdx >= curBeepN) { buzzerSet(false); beepIdx = -1; return; }
    buzzerSet(curBeep[beepIdx].on);
    beepSegEnd = millis() + curBeep[beepIdx].ms;
  }
}

void ledWrite(bool on) {
  digitalWrite(PIN_LED, (LED_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
}
int maxSeverity(const Eval& e) {
  int m = 0;
  for (int i = 0; i < NM; i++) if (e.sev[i] > m) m = e.sev[i];
  return m;
}
// LED: solid on for any alert (sev 2), slow blink for any warning (sev 1), off
// when all-clear. Purely visual — unaffected by the buzzer dismiss.
// On/off state of a seg sequence at time tMod (ms) within its own duration.
static bool segStateAt(const Seg* s, int n, uint32_t tMod) {
  uint32_t acc = 0;
  for (int i = 0; i < n; i++) { if (tMod < acc + s[i].ms) return s[i].on; acc += s[i].ms; }
  return false;
}
static uint32_t segTotal(const Seg* s, int n) { uint32_t a = 0; for (int i = 0; i < n; i++) a += s[i].ms; return a; }

// Render a LED behaviour (4.3) at time t.
static bool ledOnFor(uint8_t mode, uint32_t t) {
  switch (mode) {
    case LED_OFF:   return false;
    case LED_SOLID: return true;
    case LED_SLOW:  return (t / 500) & 1;     // ~1 Hz
    case LED_FAST:  return (t / 150) & 1;     // ~3 Hz
    case LED_SOS: { const int n = (int)(sizeof(P_SOS) / sizeof(P_SOS[0]));
                    uint32_t tot = segTotal(P_SOS, n) + 600;   // +gap so it repeats cleanly
                    return segStateAt(P_SOS, n, t % tot); }
  }
  return false;
}
// LED tracks severity with the user-chosen behaviour for warning vs alert.
void updateLed() {
  if (!settingsLed()) { ledWrite(false); return; }   // warning LED disabled
  int sev = maxSeverity(lastE);
  uint8_t mode = sev >= 2 ? settingsLedAlert() : (sev == 1 ? settingsLedWarn() : (uint8_t)LED_OFF);
  ledWrite(ledOnFor(mode, millis()));
}

// OLED power: honour the display on/off setting. Keeps the panel physically off
// (DISPLAYOFF) when disabled — the device keeps sensing, logging and serving.
void applyDisplayPower() {
  static int applied = -1;                       // -1 = unknown, 0 off, 1 on
  if (!okOled) return;
  int want = settingsDisplay() ? 1 : 0;
  if (want == applied) return;
  applied = want;
  display.ssd1306_command(want ? SSD1306_DISPLAYON : SSD1306_DISPLAYOFF);
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
// Compensation ticks the SGP41 wants (default 0x8000 == 50 %RH / 25 C).
static inline uint16_t rhTicks(float rh) { return (uint16_t)lroundf(rh * 65535.0f / 100.0f); }
static inline uint16_t tTicks(float tC)  { return (uint16_t)lroundf((tC + 45.0f) * 65535.0f / 175.0f); }

// MQ-2: average GAS_SAMPLES raw ADC reads on D0 (through the R2/R1 divider).
int readGas() {
  long acc = 0;
  for (int i = 0; i < GAS_SAMPLES; i++) acc += analogRead(PIN_MQ2_AO);
  return (int)(acc / GAS_SAMPLES);
}

Reading readSensors() {
  Reading r;
  for (int i = 0; i < NM; i++) { r.ok[i] = false; r.v[i] = NAN; }

  // 1) Temp/RH — SHT40 is authoritative, else the HTU21D breakout.
  if (okSht) {
    float t, h;
    if (sht4x.measureHighPrecision(t, h) == 0 && !isnan(t) && !isnan(h)) {
      r.v[M_TEMP] = t; r.v[M_HUM] = h; r.ok[M_TEMP] = r.ok[M_HUM] = true;
    }
  } else if (okHtu) {
    float t = htu.readTemperature(), h = htu.readHumidity();
    if (!isnan(t) && !isnan(h)) {
      r.v[M_TEMP] = t; r.v[M_HUM] = h; r.ok[M_TEMP] = r.ok[M_HUM] = true;
    }
  }

  // 2) SCD41 periodic measurement (~1 sample / 5 s); hold last-good between.
  if (okScd) {
    bool ready = false;
    if (scd4x.getDataReadyStatus(ready) == 0 && ready) {
      uint16_t co2; float t, h;
      if (scd4x.readMeasurement(co2, t, h) == 0 && co2 != 0) { lastCo2 = co2; lastScdT = t; lastScdH = h; }
    }
    r.v[M_CO2] = lastCo2; r.ok[M_CO2] = (lastCo2 >= 0);
    // Last-resort T/RH: the SCD41's own (self-heated, less accurate) values.
    if (!r.ok[M_TEMP] && !isnan(lastScdT)) {
      r.v[M_TEMP] = lastScdT; r.v[M_HUM] = lastScdH; r.ok[M_TEMP] = r.ok[M_HUM] = true;
    }
  }

  // 3) SGP41 raw -> gas index, compensated with whatever T/RH we have.
  if (okSgp) {
    uint16_t rhComp = r.ok[M_HUM] ? rhTicks(r.v[M_HUM]) : 0x8000;
    uint16_t tComp  = r.ok[M_TEMP] ? tTicks(r.v[M_TEMP]) : 0x8000;
    uint16_t srawVoc = 0, srawNox = 0;
    bool sgpOk;
    if (sgpCondLeft > 0) {                      // NOx conditioning: indices not valid yet
      sgpOk = (sgp41.executeConditioning(rhComp, tComp, srawVoc) == 0);
      if (sgpOk) sgpCondLeft--;
    } else {
      sgpOk = (sgp41.measureRawSignals(rhComp, tComp, srawVoc, srawNox) == 0);
      if (sgpOk) { lastVoc = vocAlgo.process((int32_t)srawVoc); lastNox = noxAlgo.process((int32_t)srawNox); }
    }
    if (sgpOk) sgpFail = 0;
    // I2C read failed — on battery a WiFi/BLE current spike can brown the sensor rail
    // or glitch the bus. After a few misses, re-init and re-condition so the SGP41
    // recovers instead of freezing (VOC/NOx "shutting down").
    else if (++sgpFail >= 3) {
      sgp41.begin(Wire); sgpCondLeft = SGP41_CONDITION_S; sgpFail = 0;
      Serial.println("[sgp41] read failed; re-initialising");
    }
    // The algorithms report 0 while they learn their baseline -> "no reading".
    r.v[M_VOC] = lastVoc; r.ok[M_VOC] = (lastVoc > 0);
    r.v[M_NOX] = lastNox; r.ok[M_NOX] = (lastNox > 0);
#if SGP41_DEBUG
    Serial.printf("[sgp] cond=%u vocRaw=%u noxRaw=%u -> voc=%ld nox=%ld\n",
                  sgpCondLeft, srawVoc, srawNox, (long)lastVoc, (long)lastNox);
#endif
  }

  // 4) CCS811 eCO2/TVOC (DIY breakout). Feed it T/RH once a minute.
  if (okCcs) {
    if (r.ok[M_TEMP] && millis() >= ccsEnvNext) {
      ccs.setEnvironmentalData(r.v[M_HUM], r.v[M_TEMP]);
      ccsEnvNext = millis() + 60000;
    }
    if (ccs.available() && !ccs.readData()) { lastEco2 = ccs.geteCO2(); lastTvoc = ccs.getTVOC(); }
    r.v[M_ECO2] = lastEco2; r.v[M_TVOC] = lastTvoc;
    r.ok[M_ECO2] = r.ok[M_TVOC] = (lastEco2 >= 0);
  }

  // 5) MQ-2 (optional analog). Below the floor = not fitted / no 5 V rail.
#if ENABLE_MQ2
  int g = readGas();
  r.v[M_GAS] = g; r.ok[M_GAS] = (g >= GAS_PRESENT_MIN);
  if (r.ok[M_GAS]) fitted[M_GAS] = true;       // latch: show it from the first valid reading
#endif

  return r;
}

void historyInit() {
  for (int i = 0; i < NM; i++) {
    for (int b = 0; b < CHART_BINS; b++) chSeries[i][b] = NAN;
    chAccSum[i] = 0; chAccN[i] = 0;
  }
}
void historyAdd(const Reading& r, unsigned long now) {
  if (chMinuteStart == 0) chMinuteStart = now;
  for (int i = 0; i < NM; i++) if (r.ok[i]) { chAccSum[i] += r.v[i]; chAccN[i]++; }
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

uint8_t severityOf(int m, float v) {
  const KutsuThresholds& t = settingsThresholds();   // user-editable (4.1), defaults from config.h
  switch (m) {
    case M_CO2:  return v >= t.co2h  ? 2 : v >= t.co2w  ? 1 : 0;
    case M_ECO2: return v >= t.eco2h ? 2 : v >= t.eco2w ? 1 : 0;
    case M_TVOC: return v >= t.tvoch ? 2 : v >= t.tvocw ? 1 : 0;
    case M_VOC:  return v >= t.voch  ? 2 : v >= t.vocw  ? 1 : 0;
    case M_NOX:  return v >= t.noxh  ? 2 : v >= t.noxw  ? 1 : 0;
    case M_GAS:  return v >= t.gash  ? 2 : v >= t.gasw  ? 1 : 0;
    case M_TEMP:
      if (v > t.temp_hh || v < t.temp_hl) return 2;
      return v > t.temp_wh ? 1 : 0;
    case M_HUM:
      if (v < t.hum_hl || v > t.hum_hh) return 2;
      return (v < t.hum_wl || v > t.hum_wh) ? 1 : 0;
  }
  return 0;
}

Eval evaluate(const Reading& r) {
  Eval e;
  e.harmful = false; e.alarmMetric = -1;
  for (int i = 0; i < NM; i++) e.sev[i] = r.ok[i] ? severityOf(i, r.v[i]) : 0;
  for (int k = 0; k < NM; k++) {
    int m = ALARM_PRIORITY[k];
    if (e.sev[m] == 2) { e.harmful = true; e.alarmMetric = m; break; }
  }
  return e;
}

String fmtValue(int m, float v) {
  return MD[m].dp ? String(v, (unsigned int)MD[m].dp) : String((long)lroundf(v));
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
// Drawing always goes to the RAM buffer; only the I2C flush needs a display.
void flush() { if (okOled) display.display(); }

void centerText(const char* s, int y, int size) {
  display.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  display.setCursor((OLED_W - (int)w) / 2, y);
  display.print(s);
}
void beginScreen() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
}

void showWelcome() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.drawRoundRect(0, 0, OLED_W, OLED_H, 6, SSD1306_WHITE);
  centerText("KUTSU", 7, 2);
  centerText("6th Sense", 28, 1);
  display.drawFastHLine(24, 40, OLED_W - 48, SSD1306_WHITE);
  centerText("By Papathought", 49, 1);
  flush();
}

// Second boot screen: a live, scrolling log of detection/init results.
const int BOOT_ROWS = 7;             // header + 7 lines at 8 px pitch = 64 px
String    bootLbl[BOOT_ROWS], bootSt[BOOT_ROWS];
int       bootCount = 0;
void bootLine(const String& label, const String& status) {
  Serial.printf("[boot] %-8s %s\n", label.c_str(), status.c_str());
  if (bootCount == BOOT_ROWS) {      // full: scroll up one line
    for (int i = 1; i < BOOT_ROWS; i++) { bootLbl[i - 1] = bootLbl[i]; bootSt[i - 1] = bootSt[i]; }
    bootCount--;
  }
  bootLbl[bootCount] = label; bootSt[bootCount] = status; bootCount++;
  beginScreen();
  display.setCursor(0, 0); display.print("Init modules:");
  for (int i = 0; i < bootCount; i++) {
    display.setCursor(0, 8 + i * 8);  display.print(bootLbl[i]);
    display.setCursor(66, 8 + i * 8); display.print(bootSt[i]);
  }
  flush();
  delay(200);                        // let each result register on screen
}

// The metrics this unit actually has, in table order.
int shownMetrics(int* idx) {
  int n = 0;
  for (int i = 0; i < NM; i++) if (fitted[i]) idx[n++] = i;
  return n;
}
// Up to 5 rows at 12 px; more rows squeeze to 64/n px (8 px for 8 metrics).
int rowPitch(int n) { return n <= 5 ? 12 : OLED_H / n; }

const int X_LABEL = 0, X_VALUE = 40, X_UNIT = 84, X_FLAG = 116;
void showLive() {
  beginScreen();
  int idx[NM], n = shownMetrics(idx);
  if (n == 0) {
    centerText("No sensors found", 20, 1);
    centerText("check the I2C bus", 34, 1);
    flush(); return;
  }
  int pitch = rowPitch(n), y0 = n <= 5 ? 2 : (OLED_H - n * pitch) / 2;
  for (int k = 0; k < n; k++) {
    int m = idx[k], y = y0 + k * pitch;
    display.setCursor(X_LABEL, y); display.print(MD[m].label);
    display.setCursor(X_VALUE, y); display.print(lastR.ok[m] ? fmtValue(m, lastR.v[m]) : String("--"));
    display.setCursor(X_UNIT, y);  display.print(MD[m].unit);
    if (lastE.sev[m]) { display.setCursor(X_FLAG, y); display.print(lastE.sev[m] == 2 ? "!!" : "!"); }
  }
  flush();
}

// Stats: one row per shown metric — "LABEL value" on the left, a 60-bin
// sparkline on the right (1 px bars).
void showCharts() {
  beginScreen();
  int idx[NM], n = shownMetrics(idx);
  if (n == 0) { centerText("No data", 28, 1); flush(); return; }
  const int sparkW = CHART_BINS, x0 = OLED_W - sparkW;      // 60 px at x = 68
  int pitch = rowPitch(n), y0 = n <= 5 ? 2 : 0, plotH = pitch - 3;
  for (int k = 0; k < n; k++) {
    int m = idx[k], top = y0 + k * pitch, plotBot = top + plotH;
    float cur = NAN;
    for (int b = 0; b < CHART_BINS; b++) if (!isnan(chSeries[m][b])) cur = chSeries[m][b];
    display.setCursor(0, top);  display.print(MD[m].label);
    display.setCursor(30, top);
    if (isnan(cur)) display.print("--"); else display.print((int)roundf(cur));
    for (int b = 0; b < CHART_BINS; b++) {
      float v = chSeries[m][b]; if (isnan(v)) continue;
      float frac = (v - MD[m].chMin) / (MD[m].chMax - MD[m].chMin);
      if (frac < 0) frac = 0; if (frac > 1) frac = 1;
      int h = (int)(frac * plotH);
      if (h > 0) display.fillRect(x0 + b, plotBot - h, 1, h, SSD1306_WHITE);
    }
  }
  flush();
}

void showAlarm(const Eval& e) {
  if (e.alarmMetric < 0) return;
  int m = e.alarmMetric;
  String detail = String(MD[m].label) + ": " + fmtValue(m, lastR.v[m]) + " " + MD[m].unit;
  beginScreen();
  centerText("!! WARNING !!", 6, 1);
  centerText(MD[m].alarmMsg, 26, 1);
  display.setTextSize(1); display.setCursor(4, 44); display.print(detail);
  flush();
}

void showNotice() {
  beginScreen();
  display.drawRoundRect(0, 0, OLED_W, OLED_H, 6, SSD1306_WHITE);
  centerText(noticeL1.c_str(), 16, 1);
  centerText(noticeL2.c_str(), 36, 1);
  flush();
}

// ---------------------------------------------------------------------------
// WiFi / BLE monitoring (passive)
// ---------------------------------------------------------------------------
// SSID, channel->frequency and RSSI are observable; an AP's client count is
// not (not in beacons) -> shown as "n/a". Distances are rough RSSI path-loss
// estimates (see config; calibrate on site). Only the C5 has a 5 GHz radio.
uint16_t chanToFreq(int ch) {
  if (ch >= 1 && ch <= 13)   return 2412 + (ch - 1) * 5;   // 2.4 GHz
  if (ch == 14)              return 2484;
  if (ch >= 32 && ch <= 177) return 5000 + ch * 5;         // 5 GHz (C5 only)
  return 0;
}
float rssiMeters(int rssi, float ref1m) {
  return powf(10.0f, (ref1m - (float)rssi) / (10.0f * PATH_LOSS_N));
}
String metersStr(float d) {                 // compact: "4.2" under 10 m, else "12"
  if (d < 0) d = 0;
  return d < 10.0f ? String(d, 1) : String((int)roundf(d));
}
String truncStr(const String& s, int maxChars) {
  if (maxChars < 1) maxChars = 1;
  return (int)s.length() <= maxChars ? s : s.substring(0, maxChars);
}
void printRight(const String& s, int y) {
  int x = OLED_W - (int)s.length() * 6; if (x < 0) x = 0;
  display.setCursor(x, y); display.print(s);
}
String dots() {
  int k = animPhase % 3 + 1;
  String s;
  for (int i = 0; i < 3; i++) s += (i < k) ? '.' : ' ';
  return s;
}

void harvestWifi(int n, unsigned long now) {
  apTotal = n;
  int idx[NET_SHOW]; int cnt = 0;
  for (int i = 0; i < n; i++) {              // keep the strongest NET_SHOW, sorted desc
    int r = WiFi.RSSI(i), p = cnt;
    while (p > 0 && WiFi.RSSI(idx[p - 1]) < r) p--;
    if (p < NET_SHOW) {
      for (int k = (cnt < NET_SHOW ? cnt : NET_SHOW - 1); k > p; k--) idx[k] = idx[k - 1];
      idx[p] = i;
      if (cnt < NET_SHOW) cnt++;
    }
  }
  for (int j = 0; j < cnt; j++) {
    int i = idx[j];
    String s = WiFi.SSID(i);
    apList[j].ssid = s.length() ? s : String("(hidden)");
    apList[j].rssi = WiFi.RSSI(i);
    apList[j].freq = chanToFreq(WiFi.channel(i));
  }
  apShown = cnt;

  if (useSD && now - lastNetLog >= LOG_SCAN_INTERVAL_MS) {
    L(g_logMtx);
    File f = SD.open(SD_NET_LOG, FILE_APPEND);
    if (f) {
      unsigned long up = (now - startMs) / 1000;
      for (int i = 0; i < n; i++) {
        uint16_t fr = chanToFreq(WiFi.channel(i));
        String ss = WiFi.SSID(i); if (!ss.length()) ss = "(hidden)";
        f.println(String(up) + ",\"" + ss + "\",\"" + WiFi.BSSIDstr(i) + "\","
                  + (fr >= 5000 ? "5G" : "2.4G") + "," + String(fr) + ","
                  + String(WiFi.channel(i)) + "," + String(WiFi.RSSI(i)) + ","
                  + String(rssiMeters(WiFi.RSSI(i), WIFI_RSSI_REF_1M), 1));
      }
      f.close();
    }
    U(g_logMtx);
    lastNetLog = now;
  }
  WiFi.scanDelete();
}

void bleUpsert(const char* mac, const char* name, int rssi, unsigned long t) {
  xSemaphoreTake(bleLock, portMAX_DELAY);
  int freeSlot = -1, found = -1;
  for (int i = 0; i < BLE_TABLE_MAX; i++) {
    if (!bleTab[i].used) { if (freeSlot < 0) freeSlot = i; continue; }
    if (strcmp(bleTab[i].mac, mac) == 0) { found = i; break; }
  }
  int s = found >= 0 ? found : freeSlot;
  if (s >= 0) {
    if (found < 0) { strncpy(bleTab[s].mac, mac, 17); bleTab[s].mac[17] = 0; bleTab[s].name[0] = 0; }
    if (name && name[0]) { strncpy(bleTab[s].name, name, 19); bleTab[s].name[19] = 0; }
    bleTab[s].rssi = rssi; bleTab[s].lastMs = t; bleTab[s].used = true;
  }
  xSemaphoreGive(bleLock);
}

class ScanCB : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice adv) override {
    String mac = adv.getAddress().toString();
    String nm  = adv.haveName() ? adv.getName() : String();
    bleUpsert(mac.c_str(), nm.c_str(), adv.getRSSI(), millis());
  }
};
ScanCB scanCb;

void bleSnapshot(unsigned long now) {
  static struct { char mac[18]; char name[20]; int rssi; } snap[BLE_TABLE_MAX];
  int total = 0;
  xSemaphoreTake(bleLock, portMAX_DELAY);
  for (int i = 0; i < BLE_TABLE_MAX; i++) {
    if (!bleTab[i].used) continue;
    if (now - bleTab[i].lastMs > BLE_STALE_MS) { bleTab[i].used = false; continue; }
    strcpy(snap[total].mac, bleTab[i].mac);
    strcpy(snap[total].name, bleTab[i].name);
    snap[total].rssi = bleTab[i].rssi;
    total++;
  }
  xSemaphoreGive(bleLock);                   // released before any SD I/O below
  devTotal = total;

  int idx[BLE_SHOW]; int cnt = 0;            // top BLE_SHOW by RSSI, sorted desc
  for (int i = 0; i < total; i++) {
    int r = snap[i].rssi, p = cnt;
    while (p > 0 && snap[idx[p - 1]].rssi < r) p--;
    if (p < BLE_SHOW) {
      for (int k = (cnt < BLE_SHOW ? cnt : BLE_SHOW - 1); k > p; k--) idx[k] = idx[k - 1];
      idx[p] = i;
      if (cnt < BLE_SHOW) cnt++;
    }
  }
  for (int j = 0; j < cnt; j++) {
    devList[j].name = snap[idx[j]].name[0] ? String(snap[idx[j]].name) : String(snap[idx[j]].mac);
    devList[j].rssi = snap[idx[j]].rssi;
  }
  devShown = cnt;

  if (useSD && now - lastDevLog >= LOG_SCAN_INTERVAL_MS) {
    L(g_logMtx);
    File f = SD.open(SD_DEV_LOG, FILE_APPEND);
    if (f) {
      unsigned long up = (now - startMs) / 1000;
      for (int i = 0; i < total; i++) {
        f.println(String(up) + ",\"" + snap[i].mac + "\",\"" + snap[i].name + "\","
                  + String(snap[i].rssi) + ","
                  + String(rssiMeters(snap[i].rssi, BLE_RSSI_REF_1M), 1));
      }
      f.close();
    }
    U(g_logMtx);
    lastDevLog = now;
  }
}

// Shown on the Networks/Devices screens when WiFi/BLE scanning is turned off.
void showScanOff(const char* what) {
  display.setCursor(0, 0);  display.print(what);
  display.setCursor(0, 16); display.print("Scanning off");
  display.setCursor(0, 36); display.print("To turn it on,");
  display.setCursor(0, 48); display.print("go to Settings");
  flush();
}

void showNetworks() {
  beginScreen();
  if (!settingsScan()) { showScanOff("Networks"); return; }
  display.setCursor(0, 0); display.print("Networks:"); display.print(apTotal);
  if (wifiScanning) display.print(dots());
  if (apShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    flush(); return;
  }
  int y = 10;
  for (int j = 0; j < apShown && y <= 55; j++) {
    ApInfo& a = apList[j];
    String right = String(a.freq >= 5000 ? "5G" : "2.4G") + " dev:n/a ~"
                   + metersStr(rssiMeters(a.rssi, WIFI_RSSI_REF_1M)) + "m";
    int rx = OLED_W - (int)right.length() * 6; if (rx < 0) rx = 0;
    display.setCursor(0, y); display.print(truncStr(a.ssid, rx / 6 - 1));
    display.setCursor(rx, y); display.print(right);
    y += 9;
  }
  flush();
}

void showDevices() {
  beginScreen();
  if (!settingsScan()) { showScanOff("Devices"); return; }
  display.setCursor(0, 0); display.print("Devices:"); display.print(devTotal);
  display.print(dots());
  if (devShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    flush(); return;
  }
  int y = 13;
  for (int j = 0; j < devShown && y <= 56; j++) {
    DevInfo& d = devList[j];
    display.setCursor(0, y); display.print(truncStr(d.name, 14));
    printRight("~" + metersStr(rssiMeters(d.rssi, BLE_RSSI_REF_1M)) + "m", y);
    y += 10;
  }
  flush();
}

// Hub / connectivity status: where the data goes (open http://<hub>/), or why not.
void showDash() {
  beginScreen();
  NetState st = netState();
  display.setCursor(0, 0); display.print("Hub");
  printRight(st == NET_ONLINE ? "ONLINE" : "OFFLINE", 0);
  display.drawFastHLine(0, 9, OLED_W, SSD1306_WHITE);
  if (st == NET_ONLINE) {
    String hub = netHub();
    display.setCursor(0, 13);
    if (kutsuGetRole() == ROLE_SLAVE) display.print(hub);            // open http://<this>/ in a browser
    else if (hub.length())            { display.print("Trying "); display.print(hub); }
    else                              { display.print("Finding hub"); display.print(dots()); }
    display.setCursor(0, 23);
    uint32_t bl = netBacklogBytes();
    if (kutsuGetRole() == ROLE_SLAVE) display.print(bl ? "Catching up " + String((bl + 1023) / 1024) + "kB" : String("Data in sync"));
    else                              display.print(netHubSetting().length() ? "(address pinned)" : "(auto-discovery)");
    display.setCursor(0, 33); display.print("IP " + netIp());
    display.setCursor(0, 43); display.print(truncStr(netSsid(), 14));
    printRight(String(netRssi()) + "dBm", 43);
  } else {
    String ssid = netSsid();
    display.setCursor(0, 13); display.print(settingsMode() == MODE_HANDHELD ? "Handheld mode" : "Stand-alone");
    display.setCursor(0, 23); display.print("WiFi: " + (ssid.length() ? truncStr(ssid, 15) : String("not set")));
    display.setCursor(0, 33);
    if      (st == NET_CONNECTING) { display.print("Joining"); display.print(dots()); }
    else if (st == NET_OFFLINE)    { uint32_t s = netRetryInS(); display.print(s ? "Retry in " + String(s) + "s" : String("Not reachable")); }
    else                           display.print("Set via USB serial");
  }
  display.setCursor(0, 54);
  display.print("Log: "); display.print(useSD ? "SD" : (useFlash ? "flash" : "off"));
  printRight(kutsuSuffix(), 54);
  flush();
}

void renderScreen() {
  switch (view) {
    case VIEW_LIVE:  showLive();     break;
    case VIEW_STATS: showCharts();   break;
    case VIEW_NET:   showNetworks(); break;
    case VIEW_BLE:   showDevices();  break;
    default:         showDash();     break;
  }
}

void serviceScans(unsigned long now) {
  if (!settingsScan()) {                         // scanning disabled: stop BLE, skip WiFi kicks
    if (bleScan && bleScan->isScanning()) bleScan->stop();   // (BLE advertising for setup stays up)
    return;
  }
  if (wifiScanning) {
    int st = WiFi.scanComplete();
    if (st >= 0)                     { harvestWifi(st, now); wifiScanning = false; }
    else if (st == WIFI_SCAN_FAILED) { wifiScanning = false; }
  }
  // Online, scans briefly take the radio off-channel, so run them rarely; and
  // never while the net task is associating (it holds the radio lock).
  unsigned long every = netWifiConnected() ? WIFI_SCAN_INTERVAL_ONLINE_MS : WIFI_SCAN_INTERVAL_MS;
  if (!wifiScanning && now - lastWifiKick >= every) {
    if (kutsuRadioTake(0)) {
      wifiScanning = (WiFi.scanNetworks(true /*async*/, true /*show_hidden*/) == WIFI_SCAN_RUNNING);
      kutsuRadioGive();
    }
    lastWifiKick = now;
  }
  // Timed BLE scan cycles: each restart clears the core's per-scan result list,
  // which otherwise grows with every advertiser seen.
  if (bleScan && !bleScan->isScanning()) bleScan->start(BLE_SCAN_CYCLE_S, nullptr, false);
}

// ---------------------------------------------------------------------------
// Logging — one JSON object per line (JSON Lines). Schema: DATA_CONTRACT.md
// ---------------------------------------------------------------------------
// buildRecordJson() is the single source of truth: the same string goes to
// SD/flash, to the master's dashboard, and over the mesh. Metrics of sensors
// not fitted on this unit are omitted; fitted-but-invalid readings are null.
static String jsonEsc(const String& s) {
  String o; o.reserve(s.length() + 2);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if ((uint8_t)c >= 0x20) o += c;
  }
  return o;
}

String buildRecordJson(const Reading& r, const Eval& e, const bool* fit, unsigned long uptimeS) {
  uint32_t ep = kutsuEpochNow();                        // 0 until NTP answers
  String s; s.reserve(360);
  s += "{\"v\":" + String(SCHEMA_VERSION);
  s += ",\"id\":\"" + String(kutsuName()) + "\"";
  s += ",\"mac\":\"" + String(kutsuMacStr()) + "\"";
  s += ",\"hw\":\"" BOARD_NAME "\"";
  s += ",\"role\":\"" + String(kutsuRoleStr(kutsuGetRole())) + "\"";
  s += ",\"room\":\"" + jsonEsc(kutsuRoom()) + "\"";
  s += ",\"up\":" + String(uptimeS);
  s += ",\"ts\":" + (ep ? String(ep) : String("null"));
  // Geolocation (from a connected phone/app/USB; null until a fix is supplied).
  if (kutsuHaveGeo()) {
    s += ",\"lat\":" + String(kutsuLat(), 6) + ",\"lon\":" + String(kutsuLon(), 6);
    s += ",\"loc_ts\":" + (kutsuLocTs() ? String(kutsuLocTs()) : String("null"));
  } else {
    s += ",\"lat\":null,\"lon\":null,\"loc_ts\":null";
  }
  s += ",\"sev\":" + String(maxSeverity(e));
  s += ",\"alarm\":" + String(e.harmful ? 1 : 0);
  s += ",\"fault\":" + String(g_fault ? 1 : 0);
  for (int i = 0; i < NM; i++) {
    if (!fit[i]) continue;
    s += ",\""; s += MD[i].key; s += "\":";
    s += r.ok[i] ? fmtValue(i, r.v[i]) : String("null");
  }
  for (int i = 0; i < NM; i++) {
    if (!fit[i]) continue;
    s += ",\"s_"; s += MD[i].key; s += "\":" + String(e.sev[i]);
  }
  s += "}";
  return s;
}

// Flash: rotate instead of wiping, so the newest FLASH_LOG_MAX..2x bytes survive.
void enforceFlashCap() {
  File f = LittleFS.open(FLASH_LOG_PATH, FILE_READ);
  if (!f) return;
  size_t sz = f.size(); f.close();
  if (sz <= FLASH_LOG_MAX) return;
  LittleFS.remove(FLASH_LOG_OLD_PATH);
  LittleFS.rename(FLASH_LOG_PATH, FLASH_LOG_OLD_PATH);
  Serial.println("[log] flash log rotated");
}
// All log writes funnel through here (the net task appends the timesync
// anchor), so the file is never written from two tasks at once.
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
void logRow(const Reading& r, const Eval& e, unsigned long uptimeS) {
  // Flash keeps 1 sample in FLASH_LOG_EVERY_N (wear + space) plus every alarm sample.
  if (!useSD && useFlash && !e.harmful && (sampleCount % FLASH_LOG_EVERY_N) != 0) return;
  appendLogLine(buildRecordJson(r, e, fitted, uptimeS));
}

// ---- Stored log read-back (kutsu_link.h: #3 SD dump over BLE) ----------------
bool kutsuLogEnabled() { return useSD || useFlash; }
size_t kutsuLogByteSize() {
  size_t sz = 0;
  L(g_logMtx);
  if (useSD) {
    File f = SD.open(SD_LOG_PATH, FILE_READ);    if (f) { sz = f.size(); f.close(); }
  } else if (useFlash) {
    File f = LittleFS.open(FLASH_LOG_PATH, FILE_READ); if (f) { sz = f.size(); f.close(); }
  }
  U(g_logMtx);
  return sz;
}
size_t kutsuLogReadChunk(size_t offset, char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  L(g_logMtx);
  File f = useSD ? SD.open(SD_LOG_PATH, FILE_READ)
                 : (useFlash ? LittleFS.open(FLASH_LOG_PATH, FILE_READ) : File());
  if (f) {
    if (offset < f.size() && f.seek(offset)) n = f.read((uint8_t*)out, cap);
    f.close();
  }
  U(g_logMtx);
  return n;
}

// ---- kutsu_link.h: snapshot + time sync -------------------------------------
String kutsuCurrentRecordJson() {
  Snapshot s; L(g_stateMtx); s = g_snap; U(g_stateMtx);
  if (!s.valid) return String("{}");
  return buildRecordJson(s.r, s.e, s.fitted, s.uptimeS);
}
// A time source (NTP, or a connected phone/USB over BLE/serial) answered: anchor
// uptime -> epoch and append a one-off timesync record so all earlier lines of this
// session become resolvable to absolute time.
void kutsuApplyTimeSync(uint32_t epoch, const char* src) {
  uint32_t up = (uint32_t)((millis() - startMs) / 1000);
  g_bootEpoch = (epoch > up) ? epoch - up : epoch;
  appendLogLine("{\"ev\":\"timesync\",\"id\":\"" + String(kutsuName()) +
                "\",\"up\":" + String(up) + ",\"ts\":" + String(epoch) +
                ",\"src\":\"" + String(src ? src : "ntp") + "\"}");
  Serial.printf("[time] sync src=%s epoch=%lu\n", src ? src : "ntp", (unsigned long)epoch);
}

// A connected phone / app / USB host supplies a geolocation fix (the device has no
// GPS). Stored and stamped onto every later record; also logged as a one-off anchor.
void kutsuApplyGeo(double lat, double lon) {
  g_lat = lat; g_lon = lon; g_locTs = kutsuEpochNow(); g_haveGeo = true;
  uint32_t up = (uint32_t)((millis() - startMs) / 1000);
  appendLogLine("{\"ev\":\"geo\",\"id\":\"" + String(kutsuName()) + "\",\"up\":" + String(up) +
                ",\"lat\":" + String(lat, 6) + ",\"lon\":" + String(lon, 6) +
                ",\"loc_ts\":" + (g_locTs ? String(g_locTs) : String("null")) + "}");
  Serial.printf("[geo] fix lat=%.6f lon=%.6f\n", lat, lon);
}
bool     kutsuHaveGeo() { return g_haveGeo; }
double   kutsuLat()     { return g_lat; }
double   kutsuLon()     { return g_lon; }
uint32_t kutsuLocTs()   { return g_locTs; }

// ---------------------------------------------------------------------------
// USB serial console — WiFi credentials without reflashing
// ---------------------------------------------------------------------------
String conLine, pendSsid, pendPass;
bool   havePendSsid = false;

void printStatus() {
  Serial.printf("[status] %s  %s  mac=%s  up=%lus\n", kutsuName(), BOARD_NAME, kutsuMacStr(), (millis() - startMs) / 1000);
  Serial.printf("[status] net=%s ssid=\"%s\" ip=%s role=%s room=\"%s\" time=%s\n",
                netStateStr(netState()), netSsid().c_str(), netIp().c_str(),
                kutsuRoleStr(kutsuGetRole()), kutsuRoom().c_str(), kutsuTimeSynced() ? "synced" : "uptime");
  Serial.printf("[status] mode=%s hub=%s (%s) backlog=%luB\n", settingsModeStr(),
                netHub().length() ? netHub().c_str() : "-", netHubSetting().length() ? "pinned" : "auto",
                (unsigned long)netBacklogBytes());
  Serial.print("[status] sensors:");
  if (okScd)         Serial.print(" SCD41");
  if (okSgp)         Serial.print(" SGP41");
  if (okSht)         Serial.print(" SHT40");
  if (okCcs)         Serial.print(" CCS811");
  if (okHtu)         Serial.print(" HTU21D");
  if (fitted[M_GAS]) Serial.print(" MQ-2");
  Serial.printf("  display=%s  log=%s\n", okOled ? "yes" : "no", useSD ? "SD" : (useFlash ? "flash" : "off"));
  Serial.println("[status] " + kutsuCurrentRecordJson());
}

void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String arg = sp < 0 ? String() : line.substring(sp + 1);
  cmd.toLowerCase();
  if (cmd == "ssid")      { pendSsid = arg; havePendSsid = true; Serial.println("[con] ssid set; add `pass ...` if needed, then `connect`"); }
  else if (cmd == "pass") { pendPass = arg; Serial.println("[con] password set; `connect` to apply"); }
  else if (cmd == "room") {
    Preferences p; p.begin(NVS_NAMESPACE, false); p.putString("room", arg); p.end();
    kutsuSetRoom(arg.c_str());
    Serial.printf("[con] room = \"%s\"\n", arg.c_str());
  }
  else if (cmd == "connect") {
    if (!havePendSsid) { Serial.println("[con] give `ssid <name>` first"); return; }
    netProvision(pendSsid.c_str(), pendPass.c_str(), nullptr);
    Serial.printf("[con] saved; joining \"%s\"\n", pendSsid.c_str());
    havePendSsid = false; pendPass = "";
  }
  else if (cmd == "forget") { netProvision("", "", nullptr); Serial.println("[con] WiFi disabled (handheld only)"); }
  else if (cmd == "hub") {                         // hub <host[:port]> | hub auto
    if (!arg.length()) { Serial.printf("[con] hub setting \"%s\" (empty = auto), in use \"%s\"\n", netHubSetting().c_str(), netHub().c_str()); return; }
    netSetHub(arg.c_str());
    Serial.printf("[con] hub = %s\n", netHubSetting().length() ? netHubSetting().c_str() : "auto-discover");
  }
  else if (cmd == "status") printStatus();
  // ---- time + geolocation from the USB host ------------------------------
  else if (cmd == "time") { uint32_t e = (uint32_t)strtoul(arg.c_str(), nullptr, 10);
    if (e > 1700000000UL) { kutsuApplyTimeSync(e, "usb"); Serial.println("[con] time set"); }
    else Serial.println("[con] time <unix-epoch-seconds>"); }
  else if (cmd == "geo") {                         // geo <lat> <lon>
    int sp2 = arg.indexOf(' ');
    if (sp2 > 0) { double la = arg.substring(0, sp2).toDouble(), lo = arg.substring(sp2 + 1).toDouble();
      kutsuApplyGeo(la, lo); Serial.println("[con] geo set"); }
    else Serial.println("[con] geo <lat> <lon>"); }
  // ---- device settings (buzzer/LED/display/scan/mode) --------------------
  else if (cmd == "settings") { Serial.println("[settings] " + settingsJson()); }
  else if (cmd == "mode") {
    arg.toLowerCase();
    if (arg == "handheld")        settingsSetMode(MODE_HANDHELD);
    else if (arg == "stationary") settingsSetMode(MODE_STATIONARY);
    else { Serial.println("[con] mode <handheld|stationary>"); return; }
    Serial.println("[settings] " + settingsJson());
  }
  else if (cmd == "set") {                         // set <buzzer|led|display|scan> <on|off>
    int s2 = arg.indexOf(' ');
    String field = s2 < 0 ? arg : arg.substring(0, s2);
    String val   = s2 < 0 ? String() : arg.substring(s2 + 1);
    field.toLowerCase(); val.toLowerCase();
    bool on = (val == "on" || val == "1" || val == "true");
    bool okv = (val == "on" || val == "off" || val == "1" || val == "0" || val == "true" || val == "false");
    if      (okv && field == "buzzer")  settingsSetBuzzer(on);
    else if (okv && field == "led")     settingsSetLed(on);
    else if (okv && field == "display") settingsSetDisplay(on);
    else if (okv && field == "scan")    settingsSetScan(on);
    else { Serial.println("[con] set <buzzer|led|display|scan> <on|off>"); return; }
    Serial.println("[settings] " + settingsJson());
  }
  // ---- name / buzzer-signal / LED behaviour / thresholds (4.x) -----------
  else if (cmd == "name") { kutsuSetName(arg.c_str());            // "" reverts to the creature name
    Serial.printf("[con] name = %s\n", kutsuName()); Serial.println("[settings] " + settingsJson()); }
  else if (cmd == "buzzer") { int n = arg.toInt();               // 0=triple 1=single 2=long 3=rapid 4=SOS
    settingsSetBuzzerSig((uint8_t)n); Serial.println("[settings] " + settingsJson()); }
  else if (cmd == "led") {                                       // led <warn|alert> <0-4>
    int s3 = arg.indexOf(' '); String which = s3 < 0 ? arg : arg.substring(0, s3);
    int n = s3 < 0 ? -1 : arg.substring(s3 + 1).toInt(); which.toLowerCase();
    if      (which == "warn"  && n >= 0) settingsSetLedWarn((uint8_t)n);
    else if (which == "alert" && n >= 0) settingsSetLedAlert((uint8_t)n);
    else { Serial.println("[con] led <warn|alert> <0off 1slow 2fast 3solid 4sos>"); return; }
    Serial.println("[settings] " + settingsJson()); }
  else if (cmd == "thresholds") { Serial.println("[thresholds] " + settingsThresholdsJson()); }
  else if (cmd == "threshold") {                                 // threshold <metric> <warn> <harm>
    int a = arg.indexOf(' '), b = a < 0 ? -1 : arg.indexOf(' ', a + 1);
    if (a < 0 || b < 0) { Serial.println("[con] threshold <co2|eco2|tvoc|voc|nox|gas> <warn> <harm>  (temp/hum: use the app)"); return; }
    String met = arg.substring(0, a); String w = arg.substring(a + 1, b); String h = arg.substring(b + 1);
    String j = "{\"" + met + "\":{\"w\":" + w + ",\"h\":" + h + "}}";
    if (settingsApplyThresholdsJson(j.c_str())) Serial.println("[thresholds] " + settingsThresholdsJson());
    else Serial.println("[con] bad metric/values"); }
  else Serial.println("[con] commands: ssid <name> | pass <password> | connect | forget | room <label> | hub <host[:port]|auto> | "
                      "status | settings | set <buzzer|led|display|scan> <on|off> | mode <handheld|stationary> | "
                      "time <epoch> | geo <lat> <lon> | name <text> | buzzer <0-4> | led <warn|alert> <0-4> | "
                      "thresholds | threshold <metric> <warn> <harm>");
}

void serviceConsole() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') { handleCommand(conLine); conLine = ""; }
    else if (conLine.length() < 128) conLine += c;
  }
}

// ---------------------------------------------------------------------------
// Buttons
// ---------------------------------------------------------------------------
void handleButtons() {
  bool b1 = digitalRead(PIN_BUTTON);
  if (btnPrev == HIGH && b1 == LOW) {
    noticeEnd = 0;                             // any press clears the banner
    if (lastE.harmful && !alarmAcked) {
      // Press during an active alarm -> dismiss: silence the buzzer, drop the
      // warning screen, back to live. (The LED keeps indicating the condition.)
      alarmAcked   = true;
      alarmShowing = false;
      alarmPhaseEnd = 0;
      stopBeep();
      view = VIEW_LIVE;
      renderScreen();
    } else {                                   // live -> stats -> net -> devices -> dashboard
      view = (view + 1) % NUM_VIEWS;
      if (!alarmShowing) renderScreen();
    }
  }
  btnPrev = b1;
}

// ---------------------------------------------------------------------------
// Setup: detect + init
// ---------------------------------------------------------------------------
const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "PANIC (exception)";
    case ESP_RST_INT_WDT:   return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK WATCHDOG";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT (supply dip)";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_USB:       return "USB";
    default:                return "unknown";
  }
}

String hex2(uint8_t a) { char b[6]; snprintf(b, sizeof b, "0x%02X", a); return String(b); }

void scanI2C() {
  Serial.printf("[i2c] SDA=GPIO%d SCL=GPIO%d, scanning...\n", PIN_SDA, PIN_SCL);
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    i2cFound[a] = (Wire.endTransmission() == 0);
    if (i2cFound[a]) Serial.printf("  found 0x%02X\n", a);
  }
  i2cFound[0] = i2cFound[127] = false;
}
uint8_t firstFound(uint8_t a, uint8_t b) { return i2cFound[a] ? a : (i2cFound[b] ? b : 0); }

void setup() {
  Serial.begin(115200);
  delay(1500);                       // let USB CDC enumerate so early logs aren't lost
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[main] KUTSU Sense v%s on %s, last reset: %s (%d)\n", FIRMWARE_VERSION, BOARD_NAME, resetReasonStr(rr), (int)rr);
  identityInit();
  Serial.printf("[main] %s  mac=%s\n", kutsuName(), kutsuMacStr());
  settingsInit();                      // load user toggles (buzzer/LED/display/scan/mode) from NVS
  g_stateMtx = xSemaphoreCreateMutex();
  g_logMtx   = xSemaphoreCreateMutex();
  g_radioMtx = xSemaphoreCreateMutex();

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUTTON2, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT); ledWrite(false);
#if BUZZER_TONE_HZ > 0
  ledcAttach(PIN_BUZZER, BUZZER_TONE_HZ, 8);
#else
  pinMode(PIN_BUZZER, OUTPUT);
#endif
  buzzerSet(false);
  analogReadResolution(12);          // MQ-2 on D0: 0-4095

#if defined(CONFIG_IDF_TARGET_ESP32C6)
  // The C6 variant already enabled the RF switch on the on-board antenna;
  // re-assert it and honour the external-antenna option.
  pinMode(RF_SWITCH_EN_GPIO, OUTPUT); digitalWrite(RF_SWITCH_EN_GPIO, LOW);
  pinMode(RF_ANT_SEL_GPIO, OUTPUT);   digitalWrite(RF_ANT_SEL_GPIO, RF_USE_EXTERNAL_ANT ? HIGH : LOW);
#endif

  for (int i = 0; i < NM; i++) fitted[i] = false;

  // ---- I2C scan: decides what gets initialised and shown ----
  Wire.begin(PIN_SDA, PIN_SCL); Wire.setClock(I2C_HZ);
  scanI2C();

  // ---- Display: on-board panel or plug-in module (both SSD1306, I2C) ----
  uint8_t addrOled = firstFound(ADDR_SSD1306, ADDR_SSD1306_ALT);
  okOled = (addrOled != 0);
  // begin() always runs so the frame buffer exists even with no display;
  // periphBegin=false keeps our Wire pins/clock.
  display.begin(SSD1306_SWITCHCAPVCC, okOled ? addrOled : ADDR_SSD1306, true, false);
  display.setRotation(DISPLAY_ROTATION);
  display.cp437(true);
  if (!okOled) Serial.println("[display] no SSD1306 at 0x3C/0x3D; running headless");
  showWelcome();
  delay(1800);

  bootLine("Board", BOARD_SHORT);
  bootLine("Display", okOled ? hex2(addrOled) : String("none"));

  // ---- SHT40 (temp/RH, on-board) ----
  uint8_t aSht = firstFound(ADDR_SHT40, ADDR_SHT40_ALT);
  if (aSht) {
    sht4x.begin(Wire, aSht);
    uint32_t sn; okSht = (sht4x.serialNumber(sn) == 0);
    bootLine("SHT40", okSht ? "OK" : "FAIL");
  }
  // ---- HTU21D (temp/RH, DIY breakout) — only used without an SHT40 ----
  if (i2cFound[ADDR_HTU21D]) {
    okHtu = htu.begin(&Wire);
    bootLine("HTU21D", okHtu ? (okSht ? "OK (spare)" : "OK") : "FAIL");
  }
  // ---- SGP41 (VOC/NOx, on-board) ----
  if (i2cFound[ADDR_SGP41]) {
    sgp41.begin(Wire);
    uint16_t test; okSgp = (sgp41.executeSelfTest(test) == 0 && (test & 0x0003) == 0);
    bootLine("SGP41", okSgp ? "OK" : "FAIL");
  }
  // ---- SCD41 (CO2, on-board) ----
  if (i2cFound[ADDR_SCD41]) {
    scd4x.begin(Wire, ADDR_SCD41);
    scd4x.stopPeriodicMeasurement();             // in case it was running (waits 500 ms)
    okScd = (scd4x.startPeriodicMeasurement() == 0);
    bootLine("SCD41", okScd ? "OK" : "FAIL");
  }
  // ---- CCS811 (eCO2/TVOC, DIY breakout; WAK must be tied to GND) ----
  uint8_t aCcs = firstFound(ADDR_CCS811, ADDR_CCS811_ALT);
  if (aCcs) {
    okCcs = ccs.begin(aCcs, &Wire);
    bootLine("CCS811", okCcs ? "OK" : "FAIL");
  }
  // ---- Anything else on the bus ----
  for (uint8_t a = 1; a < 127; a++) {
    if (!i2cFound[a]) continue;
    if (a == addrOled || a == aSht || a == ADDR_HTU21D || a == ADDR_SGP41 || a == ADDR_SCD41 || a == aCcs) continue;
    bootLine(hex2(a), "unknown");
  }

  fitted[M_CO2]  = okScd;
  fitted[M_ECO2] = fitted[M_TVOC] = okCcs;
  fitted[M_VOC]  = fitted[M_NOX]  = okSgp;
  fitted[M_TEMP] = fitted[M_HUM]  = (okSht || okHtu || okScd);
#if ENABLE_MQ2
  { int g = readGas(); fitted[M_GAS] = (g >= GAS_PRESENT_MIN);
    bootLine("MQ-2", fitted[M_GAS] ? "OK" : "absent"); }
#endif
  bool anyEnv = okScd || okCcs || okSgp || okSht || okHtu;
  g_fault = (aSht && !okSht) || (i2cFound[ADDR_HTU21D] && !okHtu) || (i2cFound[ADDR_SGP41] && !okSgp) ||
            (i2cFound[ADDR_SCD41] && !okScd) || (aCcs && !okCcs) || !anyEnv;
  if (!anyEnv) bootLine("Sensors", "NONE");

  // ---- Storage: MicroSD if a card is present, else internal flash ----
  spiSD.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  useSD = SD.begin(PIN_SD_CS, spiSD, SD_HZ);
  if (useSD) {
    File f;
    if (!SD.exists(SD_NET_LOG) && (f = SD.open(SD_NET_LOG, FILE_WRITE))) { f.println(NET_LOG_HEADER); f.close(); }
    if (!SD.exists(SD_DEV_LOG) && (f = SD.open(SD_DEV_LOG, FILE_WRITE))) { f.println(DEV_LOG_HEADER); f.close(); }
  } else {
    useFlash = LittleFS.begin(true);
  }
  bootLine("Logging", useSD ? "SD" : (useFlash ? "Flash" : "none"));

  // ---- Radios: passive scanning now; the net task decides about WiFi ----
  WiFi.setHostname(kutsuHostname());
  WiFi.mode(WIFI_STA);                       // station mode; joined only if creds exist
#if SOC_WIFI_SUPPORT_5G
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);     // C5: scan/join 2.4 + 5 GHz
#endif
  // Cap WiFi TX power. Default (~19.5 dBm) draws large current spikes that brown out
  // the 3 V LiPo rail (resets / sensor glitches on battery); ~13 dBm is plenty for a
  // local network and roughly halves the peak draw. Raise if range suffers on mains.
  WiFi.setTxPower(WIFI_POWER_13dBm);
  WiFi.disconnect(false);

  bleLock = xSemaphoreCreateMutex();
  // Init WITHOUT a device name. Passing a name here makes NimBLE's ble_svc_gap_init
  // assert (ble_svc_gap.c:611, rc==0) on the C5/C6 when a GATT server is also built
  // in — a boot loop. The advertised name "Kutsu-XXXXXX" is set on the advertising
  // object in bleProvisioningStart() instead, so the device still shows up by name.
  BLEDevice::init("");

  // BLE provisioning GATT server (set up WiFi/room + settings from the Kutsu app
  // over Bluetooth). Brought up BEFORE the passive scanner: establishing the GATT
  // server + advertising first is more stable than adding it to a running scan.
  // No-op when PROVISION_BLE is 0. See ble_provisioning.cpp / PROVISIONING.md.
  bleProvisioningStart();
  bootLine("BLE setup", PROVISION_BLE ? "OK" : "off");

  bleScan = BLEDevice::getScan();
  bleScan->setAdvertisedDeviceCallbacks(&scanCb, true /*wantDuplicates: keep RSSI fresh*/);
  bleScan->setActiveScan(true);              // request scan responses -> device names
  bleScan->setInterval(BLE_SCAN_INTERVAL_MS);
  bleScan->setWindow(BLE_SCAN_WINDOW_MS);
  bleScan->start(BLE_SCAN_CYCLE_S, nullptr, false);
  bootLine("BLE scan", "OK");

  netStart();                                // background: join WiFi (if set) -> dashboard
  delay(50);
  String ssid = netSsid();
  bootLine("WiFi", ssid.length() ? "joining" : "off");
  bootLine("ID", kutsuSuffix());

  historyInit();
  delay(1500);                               // hold the boot screen
  startMs = millis();
  nextTick = millis();
  lastR = readSensors();
  lastE = evaluate(lastR);
  Serial.println("[main] ready. Type `status` or `help` on this console.");
}

// ---------------------------------------------------------------------------
// Loop: 1 Hz tick
// ---------------------------------------------------------------------------
void loop() {
  unsigned long now = millis();
  applyDisplayPower();
  updateBuzzer();
  updateLed();
  handleButtons();
  serviceScans(now);
  serviceConsole();

  if (now >= nextTick) {
    nextTick += SAMPLE_INTERVAL_MS;
    unsigned long uptimeS = (now - startMs) / 1000;
    animPhase++;
    sampleCount++;
    bleSnapshot(now);

    lastR = readSensors();
    historyAdd(lastR, now);
    lastE = evaluate(lastR);

    // Publish the completed sample for the net/web tasks (one bump per tick).
    L(g_stateMtx);
    g_snap.r = lastR; g_snap.e = lastE; g_snap.uptimeS = uptimeS; g_snap.valid = true;
    for (int i = 0; i < NM; i++) g_snap.fitted[i] = fitted[i];
    U(g_stateMtx);
    g_sampleSeq = g_sampleSeq + 1;

    // Announce the dashboard once each time the unit comes online.
    NetState ns = netState();
    if (ns == NET_ONLINE && kutsuGetRole() == ROLE_SLAVE && prevNet != NET_ONLINE) {
      noticeL1 = "Hub connected";
      noticeL2 = kutsuMasterIp();
      noticeEnd = now + NOTICE_MS;
      prevNet = ns;
    } else if (ns != NET_ONLINE) {
      prevNet = ns;
    }

    // Harmful -> beep once (latched) and pulse the alarm screen: 5 s on, 1 s gap
    // showing live readings, repeat while the condition persists. A button press
    // acknowledges it (alarmAcked) -> silent + normal screens until it clears.
    // (The warning LED tracks severity independently, via updateLed().)
    if (lastE.harmful) {
      if (!alarmAcked) {
        if (!warningLatched) { warningLatched = true; startBeep(); }
        if (!alarmShowing && now >= alarmPhaseEnd) {
          alarmShowing = true; alarmPhaseEnd = now + ALARM_SHOW_MS; showAlarm(lastE);
        } else if (alarmShowing && now >= alarmPhaseEnd) {
          alarmShowing = false; alarmPhaseEnd = now + ALARM_GAP_MS; renderScreen();
        } else if (!alarmShowing) {
          renderScreen();
        }
      } else {
        renderScreen();
      }
    } else {
      alarmShowing = false; alarmPhaseEnd = 0; warningLatched = false; alarmAcked = false;
      if (now < noticeEnd) showNotice(); else renderScreen();
    }
    // Log at the user-set cadence (#6); always log an alarm sample for detail.
    bool logDue = (now - lastLogMs) >= settingsSampleMs();
    if (logDue || lastE.harmful) logRow(lastR, lastE, uptimeS);
    if (logDue) lastLogMs = now;
  }
  delay(5);
}
