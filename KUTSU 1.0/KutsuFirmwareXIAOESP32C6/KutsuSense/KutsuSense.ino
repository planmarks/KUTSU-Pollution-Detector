/* ===========================================================================
 * KUTSU Sense — firmware for the Seeed XIAO ESP32-C6 (Arduino framework)
 * "PRE-ASSEMBLED" BUILD (new on-board Sensirion sensors)
 * ---------------------------------------------------------------------------
 * Environmental monitor: true NDIR CO2 (SCD41), VOC + NOx index (SGP41),
 * temperature + humidity (SHT40). Shows them on a 128x64 I2C OLED; charts the
 * last hour; raises a pulsing full-screen alarm + 3-beep buzzer + warning LED
 * on harmful readings; logs CSV to MicroSD (falls back to internal flash).
 * Also scans nearby WiFi APs (2.4 GHz) and BLE devices on two dedicated
 * screens.
 *
 * TWO BUILDS OF THIS PRODUCT:
 *   - ../../KutsuXIAOESP32C3 .. DIY branch: solder-on HTU21D + CCS811 modules,
 *                               with NFC. Left UNTOUCHED.
 *   - THIS folder ............. pre-assembled branch: on-board SCD41/SHT40/SGP41,
 *                               NFC removed, warning LED added.
 *
 * SEE ALSO:
 *   - config.h ............ ALL tunables: pins, addresses, thresholds
 *   - README.md ........... board package + Arduino library install + wiring
 *
 * HARDWARE (pins/addresses in config.h; source of truth = board netlist
 * kutsu-board-xiao-esp32c5c6.net). I2C bus @ D4/D5 (on-board R10/R11 pull-ups).
 * Devices: OLED 0x3C, SHT40 0x44, SGP41 0x59, SCD41 0x62. MicroSD on SPI
 * (SCK=D8, MISO=D9, MOSI=D10, CS=D3). Buzzer=D2 (active). Buttons D6/D7.
 * Warning LED=D1 (active-high). C6 RF switch on GPIO3/GPIO14 (see config.h).
 *
 * LIBRARIES: Adafruit SSD1306 + GFX; Sensirion I2C SCD4x, SHT4x, SGP41; and
 * Sensirion Gas Index Algorithm; NimBLE-Arduino (2.x). WiFi/SD/LittleFS/Wire/
 * SPI are built into the ESP32 core.
 *
 * FILE MAP (top -> bottom):
 *   globals/objects .......... peripheral instances, ok-flags, last-good values
 *   Reading / Eval structs ... the per-tick data + alarm-severity result
 *   readSensors() ............ SHT40 -> SGP41(comp) -> SCD41 -> Reading
 *   history* ................. 5 metrics x 60 one-minute bins for the charts
 *   evaluate() ............... thresholds -> per-metric severity (0/1/2)
 *   updateLed()/updateBuzzer() non-blocking warning outputs
 *   show*() / liveRow ........ OLED screens: welcome, live, charts, net, devices, alarm
 *   WiFi/BLE ................. harvestWifi (async) + BLE callback/bleSnapshot + serviceScans + SD logs
 *   log*() ................... SD-then-flash CSV logging
 *   handleButtons ............ D6: dismiss active alarm, else cycle the 4 screens
 *   setup()/loop() ........... init + 1 Hz tick (read->history->evaluate->display->log)
 *
 * MEASUREMENT NOTES:
 *   - SHT40 is authoritative for Temp/Hum (display + log) and supplies the
 *     RH/T compensation fed to the SGP41.
 *   - SGP41 raw signals are converted to VOC/NOx Index by the Sensirion Gas
 *     Index Algorithm (expects 1 Hz sampling). First SGP41_CONDITION_S seconds
 *     run NOx conditioning; indices read as "warming" until then.
 *   - SCD41 runs periodic measurement (~1 sample / 5 s); CO2 holds last-good
 *     between updates. Its own T/RH is not displayed (SHT40 is used instead).
 * =========================================================================== */

#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <LittleFS.h>
#include <math.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <SensirionI2cScd4x.h>   // new-gen driver (v1.x): note the lowercase "I2c"
#include <SensirionI2cSht4x.h>   // new-gen driver (v1.x): note the lowercase "I2c"
#include <SensirionI2CSgp41.h>   // classic driver: keeps the uppercase "I2C"
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

#include "config.h"

Adafruit_SSD1306    display(OLED_W, OLED_H, &Wire, -1);
SensirionI2cScd4x   scd4x;
SensirionI2cSht4x   sht4x;
SensirionI2CSgp41   sgp41;
VOCGasIndexAlgorithm vocAlgo;   // default sampling interval = 1 s
NOxGasIndexAlgorithm noxAlgo;
SPIClass            spiSD(FSPI);

bool okScd = false, okSht = false, okSgp = false;
bool useSD = false, useFlash = false;

int32_t  lastCo2 = -1, lastVoc = -1, lastNox = -1;   // last-good values
uint16_t sgpCondLeft = SGP41_CONDITION_S;            // SGP41 conditioning countdown (1 Hz)

struct Reading {
  bool    shtOk, scdOk, sgpOk;
  int32_t co2;              // ppm (SCD41)
  int32_t vocIdx, noxIdx;   // 1..500 (SGP41 via gas index algo)
  float   tempC, hum;       // SHT40
};

struct Eval {
  int  sevCo2, sevVoc, sevNox, sevTemp, sevHum;
  bool harmful;
  const char* msg;
  String detail;
};

const int    NM = 5;
const char*  CH_LABEL[NM] = {"CO2", "VOC", "NOx", "Temp", "Hum"};
const float  CH_MIN[NM]   = {400, 0,   0,   0,  0};
const float  CH_MAX[NM]   = {2500, 500, 500, 70, 100};
float         chSeries[NM][CHART_BINS];
double        chAccSum[NM];
int           chAccN[NM];
unsigned long chMinuteStart = 0;

// Screens the button cycles through, in order.
enum { VIEW_LIVE = 0, VIEW_STATS, VIEW_NET, VIEW_BLE, NUM_VIEWS };

int           view = VIEW_LIVE;
bool          alarmShowing = false;
unsigned long alarmPhaseEnd = 0;
bool          warningLatched = false;
bool          alarmAcked = false;   // set when the button dismisses an active alarm
unsigned long nextTick = 0, startMs = 0;
bool          btnPrev = HIGH;
Reading       lastR;
Eval          lastE;

uint8_t       animPhase = 0;        // drives the "..." scan animation + LED warn-blink (1 Hz)

// ---- WiFi (async background scan) ----
struct ApInfo  { String ssid; int rssi; uint16_t freq; };
ApInfo        apList[NET_SHOW];
int           apShown = 0, apTotal = 0;
bool          wifiScanning = false;
unsigned long lastWifiKick = 0, lastNetLog = 0;

// ---- BLE (continuous background scan -> aged table, filled from a task callback) ----
struct BleEntry { char mac[18]; char name[20]; int rssi; unsigned long lastMs; bool used; };
BleEntry          bleTab[BLE_TABLE_MAX];
SemaphoreHandle_t bleLock;           // guards bleTab (callback task vs. loop)

struct DevInfo { String name; int rssi; };
DevInfo       devList[BLE_SHOW];     // loop-owned display snapshot
int           devShown = 0, devTotal = 0;
unsigned long lastDevLog = 0;

// ---------------------------------------------------------------------------
// Warning outputs: buzzer (3-beep-once) + LED (severity indicator)
// ---------------------------------------------------------------------------
struct Seg { bool on; uint16_t ms; };
const Seg BEEP[] = {                 // 3 short beeps: on/gap/on/gap/on
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

void ledWrite(bool on) {
  digitalWrite(PIN_LED, (LED_ACTIVE_HIGH ? on : !on) ? HIGH : LOW);
}
int maxSeverity(const Eval& e) {
  int m = e.sevCo2;
  if (e.sevVoc  > m) m = e.sevVoc;
  if (e.sevNox  > m) m = e.sevNox;
  if (e.sevTemp > m) m = e.sevTemp;
  if (e.sevHum  > m) m = e.sevHum;
  return m;
}
// LED: solid on for any alert (sev 2), slow blink for any warning (sev 1), off
// when all-clear. Purely visual — unaffected by the buzzer dismiss.
void updateLed() {
  int sev = maxSeverity(lastE);
  if      (sev >= 2) ledWrite(true);
  else if (sev == 1) ledWrite(animPhase & 1);   // ~0.5 Hz blink
  else               ledWrite(false);
}

// ---------------------------------------------------------------------------
// Sensors
// ---------------------------------------------------------------------------
// Compensation ticks the SGP41 wants (default 0x8000 == 50 %RH / 25 C).
static inline uint16_t rhTicks(float rh)  { return (uint16_t)lroundf(rh * 65535.0f / 100.0f); }
static inline uint16_t tTicks(float tC)   { return (uint16_t)lroundf((tC + 45.0f) * 65535.0f / 175.0f); }

Reading readSensors() {
  Reading r;
  r.shtOk = r.scdOk = r.sgpOk = false;
  r.co2 = r.vocIdx = r.noxIdx = -1;
  r.tempC = r.hum = NAN;

  // 1) SHT40 (authoritative temp/RH) — also feeds SGP41 compensation.
  uint16_t rhComp = 0x8000, tComp = 0x8000;
  if (okSht) {
    float t, h;
    if (sht4x.measureHighPrecision(t, h) == 0 && !isnan(t) && !isnan(h)) {
      r.tempC = t; r.hum = h; r.shtOk = true;
      rhComp = rhTicks(h); tComp = tTicks(t);
    }
  }

  // 2) SGP41 raw -> gas index. Conditioning for the first SGP41_CONDITION_S s.
  if (okSgp) {
    uint16_t srawVoc = 0, srawNox = 0, err;
    if (sgpCondLeft > 0) {
      err = sgp41.executeConditioning(rhComp, tComp, srawVoc);   // NOx not valid yet
      if (err == 0) sgpCondLeft--;
    } else {
      err = sgp41.measureRawSignals(rhComp, tComp, srawVoc, srawNox);
      if (err == 0) {
        lastVoc = vocAlgo.process((int32_t)srawVoc);
        lastNox = noxAlgo.process((int32_t)srawNox);
        r.sgpOk = true;
      }
    }
  }
  r.vocIdx = lastVoc; r.noxIdx = lastNox;

  // 3) SCD41 periodic measurement (~1 sample / 5 s); hold last-good otherwise.
  if (okScd) {
    bool ready = false;
    if (scd4x.getDataReadyStatus(ready) == 0 && ready) {
      uint16_t co2; float t, h;
      if (scd4x.readMeasurement(co2, t, h) == 0 && co2 != 0) lastCo2 = co2;
    }
  }
  r.co2 = lastCo2; r.scdOk = (lastCo2 >= 0);

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
    case 0:  return r.scdOk ? (float)r.co2    : NAN;
    case 1:  return r.sgpOk ? (float)r.vocIdx : NAN;
    case 2:  return r.sgpOk ? (float)r.noxIdx : NAN;
    case 3:  return r.shtOk ? r.tempC         : NAN;
    default: return r.shtOk ? r.hum           : NAN;
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
  e.sevCo2 = e.sevVoc = e.sevNox = e.sevTemp = e.sevHum = 0;
  e.harmful = false; e.msg = ""; e.detail = "";

  if (r.scdOk) {
    if (r.co2 >= CO2_HARM) e.sevCo2 = 2; else if (r.co2 >= CO2_WARN) e.sevCo2 = 1;
  }
  if (r.sgpOk) {
    if (r.vocIdx >= VOC_HARM) e.sevVoc = 2; else if (r.vocIdx >= VOC_WARN) e.sevVoc = 1;
    if (r.noxIdx >= NOX_HARM) e.sevNox = 2; else if (r.noxIdx >= NOX_WARN) e.sevNox = 1;
  }
  if (r.shtOk) {
    if (r.tempC > TEMP_HARM_HI || r.tempC < TEMP_HARM_LO) e.sevTemp = 2;
    else if (r.tempC > TEMP_WARN_HI) e.sevTemp = 1;
    if (r.hum < HUM_HARM_LO || r.hum > HUM_HARM_HI) e.sevHum = 2;
    else if (r.hum < HUM_WARN_LO || r.hum > HUM_WARN_HI) e.sevHum = 1;
  }

  e.harmful = (e.sevCo2 == 2 || e.sevVoc == 2 || e.sevNox == 2 ||
               e.sevTemp == 2 || e.sevHum == 2);
  if      (e.sevCo2  == 2) { e.msg = "HIGH CO2";        e.detail = "CO2: "  + String(r.co2)    + "ppm"; }
  else if (e.sevVoc  == 2) { e.msg = "HIGH VOC";        e.detail = "VOC: "  + String(r.vocIdx) + " idx"; }
  else if (e.sevNox  == 2) { e.msg = "HIGH NOx";        e.detail = "NOx: "  + String(r.noxIdx) + " idx"; }
  else if (e.sevTemp == 2) { e.msg = "UNSAFE TEMP";     e.detail = "Temp: " + String(r.tempC, 1) + "C"; }
  else if (e.sevHum  == 2) { e.msg = "UNSAFE HUMIDITY"; e.detail = "Hum: "  + String(r.hum, 1)  + "%"; }
  return e;
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
  bootY = 8;                        // 8px pitch fits header + 7 module lines in 64px
}
void bootLine(const char* label, const char* status) {
  display.setCursor(0, bootY);  display.print(label);
  display.setCursor(72, bootY); display.print(status);
  display.display();
  bootY += 8;
  delay(300);                     // let each result register on screen
}

const int X_LABEL = 0, X_VALUE = 40, X_UNIT = 84, X_FLAG = 116;
void liveRow(int y, const char* label, bool ok, const String& val, const char* unit, int sev) {
  display.setCursor(X_LABEL, y); display.print(label);
  display.setCursor(X_VALUE, y); display.print(ok ? val : String("--"));
  display.setCursor(X_UNIT, y);  display.print(unit);
  if (sev) { display.setCursor(X_FLAG, y); display.print(sev == 2 ? "!!" : "!"); }
}

// 5 metrics on 64px -> 12px row pitch (y = 2,14,26,38,50).
void showLive() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
  liveRow(2,  "CO2",  lastR.scdOk, String(lastR.co2),      "ppm",     lastE.sevCo2);
  liveRow(14, "VOC",  lastR.sgpOk, String(lastR.vocIdx),   "idx",     lastE.sevVoc);
  liveRow(26, "NOx",  lastR.sgpOk, String(lastR.noxIdx),   "idx",     lastE.sevNox);
  liveRow(38, "Temp", lastR.shtOk, String(lastR.tempC, 1), "\xF8""C", lastE.sevTemp);
  liveRow(50, "Hum",  lastR.shtOk, String(lastR.hum, 1),   "%RH",     lastE.sevHum);
  display.display();
}

// Stats: one compact row per metric — "LABEL value" on the left, a 60-bin
// sparkline on the right (1px bars). 5 rows at 12px pitch.
void showCharts(int page) {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE);
  const int pitch = 12, barW = 1, sparkW = CHART_BINS * barW; // 60px
  const int x0 = OLED_W - sparkW;                             // 68
  for (int i = 0; i < NM; i++) {
    int top = i * pitch + 2, plotH = 9, plotBot = top + plotH;
    float cur = NAN;
    for (int b = 0; b < CHART_BINS; b++) if (!isnan(chSeries[i][b])) cur = chSeries[i][b];
    display.setCursor(0, top);  display.print(CH_LABEL[i]);
    display.setCursor(30, top);
    if (isnan(cur)) display.print("--"); else display.print((int)roundf(cur));
    for (int b = 0; b < CHART_BINS; b++) {
      float v = chSeries[i][b]; if (isnan(v)) continue;
      float frac = (v - CH_MIN[i]) / (CH_MAX[i] - CH_MIN[i]);
      if (frac < 0) frac = 0; if (frac > 1) frac = 1;
      int h = (int)(frac * plotH), x = x0 + b * barW;
      if (h > 0) display.fillRect(x, plotBot - h, barW, h, SSD1306_WHITE);
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

// ---------------------------------------------------------------------------
// WiFi / BLE monitoring  (passive; C6 WiFi is 2.4 GHz only)
// ---------------------------------------------------------------------------
// SSID, channel->frequency (always 2.4 GHz on the C6) and RSSI are observable;
// an AP's client count is not (not in beacons) -> shown as "n/a". Distances are
// rough RSSI path-loss estimates (see config; calibrate on site). WiFi scans
// async in the background; BLE scans continuously via a task callback filling an
// aged table. Both snapshot to SD if a card is present.

uint16_t chanToFreq(int ch) {
  if (ch >= 1 && ch <= 13) return 2412 + (ch - 1) * 5;   // 2.4 GHz
  if (ch == 14)            return 2484;
  if (ch >= 32 && ch <= 177) return 5000 + ch * 5;       // 5 GHz (unused on C6)
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

class ScanCB : public NimBLEScanCallbacks {
 public:
  void onResult(const NimBLEAdvertisedDevice* adv) override {
    std::string mac = adv->getAddress().toString();
    std::string nm  = adv->getName();
    bleUpsert(mac.c_str(), nm.c_str(), adv->getRSSI(), millis());
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
    lastDevLog = now;
  }
}

void showNetworks() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
  display.setCursor(0, 0); display.print("Networks:"); display.print(apTotal);
  if (wifiScanning) display.print(dots());
  if (apShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    display.display(); return;
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
  display.display();
}

void showDevices() {
  display.clearDisplay();
  display.setTextSize(1); display.setTextColor(SSD1306_WHITE); display.cp437(true);
  display.setCursor(0, 0); display.print("Devices:"); display.print(devTotal);
  display.print(dots());
  if (devShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    display.display(); return;
  }
  int y = 13;
  for (int j = 0; j < devShown && y <= 56; j++) {
    DevInfo& d = devList[j];
    display.setCursor(0, y); display.print(truncStr(d.name, 14));
    printRight("~" + metersStr(rssiMeters(d.rssi, BLE_RSSI_REF_1M)) + "m", y);
    y += 10;
  }
  display.display();
}

void renderScreen() {
  switch (view) {
    case VIEW_LIVE:  showLive();     break;
    case VIEW_STATS: showCharts(0);  break;
    case VIEW_NET:   showNetworks(); break;
    default:         showDevices();  break;
  }
}

void serviceScans(unsigned long now) {
  if (wifiScanning) {
    int st = WiFi.scanComplete();
    if (st >= 0)                     { harvestWifi(st, now); wifiScanning = false; }
    else if (st == WIFI_SCAN_FAILED) { wifiScanning = false; }
  }
  if (!wifiScanning && now - lastWifiKick >= WIFI_SCAN_INTERVAL_MS) {
    WiFi.scanNetworks(true /*async*/, true /*show_hidden*/);
    wifiScanning = true; lastWifiKick = now;
  }
  NimBLEScan* pScan = NimBLEDevice::getScan();
  if (!pScan->isScanning()) pScan->start(0 /*continuous*/, false);
}

void ensureHeader(fs::FS& fs, const char* path, const char* header) {
  if (!fs.exists(path)) { File f = fs.open(path, FILE_WRITE); if (f) { f.println(header); f.close(); } }
}
void enforceFlashCap() {
  File f = LittleFS.open(FLASH_LOG_PATH, FILE_READ);
  if (!f) return;
  size_t sz = f.size(); f.close();
  if (sz <= FLASH_LOG_MAX) return;

  LittleFS.remove(FLASH_LOG_PATH);
  ensureHeader(LittleFS, FLASH_LOG_PATH, LOG_HEADER);
  Serial.println("[log] flash cap hit, log reset");
}
void logRow(const Reading& r, unsigned long uptimeS, bool alarm) {
  String row = String(uptimeS) + ","
    + (r.scdOk ? String(r.co2)    : "") + ","
    + (r.sgpOk ? String(r.vocIdx) : "") + ","
    + (r.sgpOk ? String(r.noxIdx) : "") + ","
    + (r.shtOk ? String(r.tempC, 1) : "") + ","
    + (r.shtOk ? String(r.hum, 1) : "") + ","
    + (alarm ? "1" : "0");
  if (useSD) {
    File f = SD.open(SD_LOG_PATH, FILE_APPEND); if (f) { f.println(row); f.close(); }
  } else if (useFlash) {
    File f = LittleFS.open(FLASH_LOG_PATH, FILE_APPEND); if (f) { f.println(row); f.close(); }
    enforceFlashCap();
  }
}

void handleButtons() {
  bool b1 = digitalRead(PIN_BUTTON);
  if (btnPrev == HIGH && b1 == LOW) {
    if (lastE.harmful && !alarmAcked) {
      // Press during an active alarm -> dismiss: silence the buzzer, drop the
      // warning screen, back to live. (The LED keeps indicating the condition.)
      alarmAcked   = true;
      alarmShowing = false;
      alarmPhaseEnd = 0;
      stopBeep();
      view = 0;
      renderScreen();
    } else {                                   // otherwise cycle: live -> stats -> net -> devices
      view = (view + 1) % NUM_VIEWS;
      if (!alarmShowing) renderScreen();
    }
  }
  btnPrev = b1;
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT); digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_LED, OUTPUT); ledWrite(false);

  // XIAO ESP32-C6 RF switch: power it (GPIO3 low) and pick the antenna (GPIO14).
  pinMode(RF_SWITCH_EN_GPIO, OUTPUT); digitalWrite(RF_SWITCH_EN_GPIO, LOW);
  pinMode(RF_ANT_SEL_GPIO, OUTPUT);   digitalWrite(RF_ANT_SEL_GPIO, RF_USE_EXTERNAL_ANT ? HIGH : LOW);

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
  delay(1800);

  bootBegin();
  bootLine("OLED", okOled ? "OK" : "FAIL");

  // ---- SHT40 (temp/RH) ----
  sht4x.begin(Wire, ADDR_SHT40);
  { uint32_t sn; okSht = (sht4x.serialNumber(sn) == 0); }
  if (!okSht) Serial.println("[sensors] SHT40 init failed");
  bootLine("SHT40", okSht ? "OK" : "FAIL");

  // ---- SGP41 (VOC/NOx) ----
  sgp41.begin(Wire);
  { uint16_t testResult; okSgp = (sgp41.executeSelfTest(testResult) == 0 && (testResult & 0x0003) == 0); }
  if (!okSgp) Serial.println("[sensors] SGP41 init/self-test failed");
  bootLine("SGP41", okSgp ? "OK" : "FAIL");

  // ---- SCD41 (CO2) ----
  scd4x.begin(Wire, ADDR_SCD41);
  scd4x.stopPeriodicMeasurement();               // in case it was already running
  okScd = (scd4x.startPeriodicMeasurement() == 0);
  if (!okScd) Serial.println("[sensors] SCD41 init failed");
  bootLine("SCD41", okScd ? "OK" : "FAIL");

  spiSD.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  useSD = SD.begin(PIN_SD_CS, spiSD, SD_HZ);
  if (useSD) {
    ensureHeader(SD, SD_LOG_PATH, LOG_HEADER);
    ensureHeader(SD, SD_NET_LOG, NET_LOG_HEADER);
    ensureHeader(SD, SD_DEV_LOG, DEV_LOG_HEADER);
    Serial.println("[log] SD card mounted");
  } else {
    Serial.println("[log] no SD card; trying flash");
    useFlash = LittleFS.begin(true);
    if (useFlash) { ensureHeader(LittleFS, FLASH_LOG_PATH, LOG_HEADER); Serial.println("[log] logging to flash"); }
    else Serial.println("[log] flash init failed; not logging");
  }
  bootLine("Logging", useSD ? "SD" : (useFlash ? "Flash" : "none"));

  WiFi.mode(WIFI_STA);              // station mode, not connected — scan only
  WiFi.disconnect(false);
  bootLine("WiFi", "OK");

  bleLock = xSemaphoreCreateMutex();
  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCb);
  pScan->setActiveScan(true);
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setDuplicateFilter(false);
  pScan->start(0 /*continuous*/, false);
  bootLine("BLE", "OK");

  historyInit();

  delay(1800);
  startMs = millis();
  nextTick = millis();
  lastR = readSensors();
  lastE = evaluate(lastR);
}

void loop() {
  unsigned long now = millis();
  updateBuzzer();
  updateLed();
  handleButtons();
  serviceScans(now);

  if (now >= nextTick) {
    nextTick += SAMPLE_INTERVAL_MS;
    unsigned long uptimeS = (now - startMs) / 1000;
    animPhase++;
    bleSnapshot(now);

    lastR = readSensors();
    historyAdd(lastR, now);
    lastE = evaluate(lastR);

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
      renderScreen();
    }
    logRow(lastR, uptimeS, lastE.harmful);
  }
  delay(5);
}
