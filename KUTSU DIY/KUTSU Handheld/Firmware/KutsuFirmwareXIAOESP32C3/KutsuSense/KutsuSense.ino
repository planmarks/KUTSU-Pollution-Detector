/* ===========================================================================
 * KUTSU Sense — firmware for the Seeed XIAO ESP32-C3 (Arduino framework)
 * ---------------------------------------------------------------------------
 * Environmental monitor: reads air quality (eCO2, TVOC),
 * temperature and humidity; shows them on a 128x64 I2C OLED; charts the last
 * hour; raises a pulsing full-screen alarm + 3-beep buzzer on harmful
 * readings; logs CSV to MicroSD (falls back to internal flash); and writes a
 * fixed NFC health record. Also scans nearby WiFi APs (2.4 GHz) and BLE devices
 * on two dedicated screens. Retargeted from the XIAO ESP32-C5 build; the C3 is
 * 2.4 GHz only (no 5 GHz radio). Descends from the original RP2040/CircuitPython
 * build (that version lives in ../../Pico).
 *
 * SEE ALSO:
 *   - config.h ............ ALL tunables: pins, thresholds, NFC data
 *   - README.md ........... board package + Arduino library install + wiring
 *   - ../../DATA_CONTRACT.md (planned) + the connectivity roadmap in the plan file
 *
 * HARDWARE (pins in config.h). I2C bus @ D4/D5 needs external 4.7k pull-ups to
 * 3V3 (the C3 has none onboard). Devices: OLED 0x3C, HTU21D 0x40,
 * CCS811 0x5A (WAK->GND!), NTAG 0x55. MicroSD on SPI (D8/D9/D10/D3).
 * Buzzer=D2 (active). Buttons D6/D7.
 * NOTE (C3 strapping pins): D8=GPIO8 and D9=GPIO9 are boot-strapping pins on the
 * ESP32-C3. If the board won't enter run mode with the MicroSD wired, unplug the
 * card while resetting, or move SCK/MISO off D8/D9 (see README §5).
 *
 * LIBRARIES: Adafruit SSD1306, GFX, HTU21DF, CCS811 (+ BusIO); NimBLE-Arduino.
 * WiFi, SD, LittleFS, Wire, SPI are built into the ESP32 core.
 *
 * FILE MAP (top -> bottom):
 *   globals/objects .......... peripheral instances, ok-flags, last-good values
 *   Reading / Eval structs ... the per-tick data + alarm-severity result
 *   readSensors() ............ all sensors -> Reading
 *   history* ................. 4 metrics x 60 one-minute bins for the charts
 *   evaluate() ............... thresholds -> per-metric severity (0/1/2)
 *   show*() / liveRow ........ OLED screens: welcome, live, charts, net, devices, alarm
 *   WiFi/BLE ................. harvestWifi (async) + BLE callback/bleSnapshot + serviceScans + SD logs
 *   log*() ................... SD-then-flash CSV logging
 *   nfc* ..................... one-shot NDEF health record to the NTAG
 *   handleButtons ............ D6: dismiss active alarm, else cycle the 4 screens
 *   setup()/loop() ........... init + 1 Hz tick (read->history->evaluate->display->log)
 *
 * DATA FLOW each 1 s tick: readSensors() -> historyAdd() -> evaluate()
 *   -> (alarm pulse 5s on / 1s gap + 3-beep-once) or renderScreen() -> logRow().
 *   The buzzer tone is non-blocking (updateBuzzer() advances it every loop); a
 *   button press dismisses an active alarm (silences the buzzer, back to live).
 *
 * KNOWN ISSUES / OPEN ITEMS (as of handoff):
 *   1. MQ-2 combustible-gas sensor REMOVED from this build (unreliable ppm
 *      conversion + no presence check on the analog pin). No gas metric.
 *   2. SGP40 VOC-index sensor REMOVED from this build (duplicated the CCS811
 *      air-quality reading and confused end users). Air quality is now the
 *      CCS811 eCO2/TVOC only, which need ~20 min run-in / 48 h burn-in.
 *   3. No RTC: log timestamps are uptime seconds. NTP is planned with Wi-Fi.
 *   4. WiFi/BLE monitoring caveats: AP client-count is NOT knowable from a
 *      passive scan -> shown as "n/a". BLE = LE only (no Classic); unnamed
 *      advertisers show their MAC. Distances are rough RSSI path-loss estimates
 *      (config: *_RSSI_REF_1M, PATH_LOSS_N). The C3 is 2.4 GHz only, so every
 *      AP shows the "2.4G" band (no 5 GHz APs will ever appear).
 *   5. Not yet built: cloud/app connectivity (BLE->app, Wi-Fi->cloud dashboard).
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
#include <Adafruit_HTU21DF.h>
#include <Adafruit_CCS811.h>

#include "config.h"

Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
Adafruit_HTU21DF htu;
Adafruit_CCS811  ccs;
SPIClass         spiSD(FSPI);

bool okHtu = false, okCcs = false;
bool useSD = false, useFlash = false;

int32_t lastEco2 = -1, lastTvoc = -1;

struct Reading {
  bool  htuOk, ccsOk;
  int32_t eco2, tvoc;
  float tempC, hum;
};

struct Eval {
  int sevTvoc, sevEco2, sevTemp, sevHum;
  bool harmful;
  const char* msg;
  String detail;
};



const int    NM = 4;
const char*  CH_LABEL[NM] = {"eCO2", "TVOC", "Temp", "Hum"};
const float  CH_MIN[NM]   = {400, 0, 0, 0};
const float  CH_MAX[NM]   = {2500, 3000, 70, 100};
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

uint8_t       animPhase = 0;        // drives the "..." scanning animation (1 Hz)

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

Reading readSensors() {
  Reading r; r.htuOk = r.ccsOk = false;
  r.eco2 = r.tvoc = -1; r.tempC = r.hum = NAN;

  if (okHtu) {
    float t = htu.readTemperature(), h = htu.readHumidity();
    if (!isnan(t) && !isnan(h)) { r.tempC = t; r.hum = h; r.htuOk = true; }
  }
  if (okCcs && ccs.available()) {
    if (!ccs.readData()) { lastEco2 = ccs.geteCO2(); lastTvoc = ccs.getTVOC(); }
  }
  r.eco2 = lastEco2; r.tvoc = lastTvoc; r.ccsOk = (lastEco2 >= 0);

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
  e.sevTvoc = e.sevEco2 = e.sevTemp = e.sevHum = 0;
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

  e.harmful = (e.sevTvoc == 2 || e.sevEco2 == 2 ||
               e.sevTemp == 2 || e.sevHum == 2);
  if      (e.sevEco2 == 2) { e.msg = "HIGH CO2";          e.detail = "eCO2: " + String(r.eco2)   + "ppm"; }
  else if (e.sevTvoc == 2) { e.msg = "HIGH TVOC";         e.detail = "TVOC: " + String(r.tvoc)   + "ppb"; }
  else if (e.sevTemp == 2) { e.msg = "UNSAFE TEMP";       e.detail = "Temp: " + String(r.tempC, 1) + "C"; }
  else if (e.sevHum  == 2) { e.msg = "UNSAFE HUMIDITY";   e.detail = "Hum: "  + String(r.hum, 1)  + "%"; }
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
  liveRow(6,  "eCO2",   lastR.ccsOk, String(lastR.eco2),     "ppm",     lastE.sevEco2);
  liveRow(20, "TVOC",   lastR.ccsOk, String(lastR.tvoc),     "ppb",     lastE.sevTvoc);
  liveRow(34, "Temp",   lastR.htuOk, String(lastR.tempC, 1), "\xF8""C", lastE.sevTemp);
  liveRow(48, "Hum",    lastR.htuOk, String(lastR.hum, 1),   "%RH",     lastE.sevHum);
  display.display();
}

const int CHARTS_PER_PAGE = NM;            // all metrics on a single stats page

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

// ---------------------------------------------------------------------------
// WiFi / BLE monitoring
// ---------------------------------------------------------------------------
// WiFi APs and BLE advertisers are only *observed* passively. Accurate: SSID,
// channel->frequency (2.4 GHz only on the C3), RSSI. Not knowable from a passive
// scan: the client count of someone else's AP (not in beacons) -> shown as "n/a".
// Distances are rough RSSI path-loss estimates (see config; calibrate on site).
// WiFi scans async in the background; BLE scans continuously via a task callback
// that fills an aged table. Both snapshot to SD (if a card is present).

uint16_t chanToFreq(int ch) {
  if (ch >= 1 && ch <= 13) return 2412 + (ch - 1) * 5;   // 2.4 GHz
  if (ch == 14)            return 2484;
  if (ch >= 32 && ch <= 177) return 5000 + ch * 5;       // 5 GHz
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
// Right-align a string ending at OLED_W on row y (6px per char at text size 1).
void printRight(const String& s, int y) {
  int x = OLED_W - (int)s.length() * 6; if (x < 0) x = 0;
  display.setCursor(x, y); display.print(s);
}
// Animated dots for "scanning..." feedback: 1..3 dots, right-padded to width 3.
String dots() {
  int k = animPhase % 3 + 1;
  String s;
  for (int i = 0; i < 3; i++) s += (i < k) ? '.' : ' ';
  return s;
}

// Harvest a finished async WiFi scan: top NET_SHOW into apList (display), and a
// full snapshot of all APs to SD every LOG_SCAN_INTERVAL_MS (if a card is present).
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

// Upsert one BLE advertisement into the aged table (called from the scan task).
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

// Snapshot the BLE table (loop thread): prune stale, count total, pick the top
// BLE_SHOW for display, and log a full snapshot to SD every LOG_SCAN_INTERVAL_MS.
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
  if (wifiScanning) display.print(dots());          // animated feedback while scanning
  if (apShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    display.display(); return;
  }
  int y = 10;                                        // one line per AP: name band dev:n/a dist
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
  display.print(dots());                             // BLE scans continuously -> always animate
  if (devShown == 0) {
    display.setCursor(0, 28); display.print("Scanning"); display.print(dots());
    display.display(); return;
  }
  int y = 13;                                        // one row per device: name + distance
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

// Background WiFi: async scan every WIFI_SCAN_INTERVAL_MS (non-blocking), harvested
// when complete. BLE runs continuously via its task callback; we just keep it alive.
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
    + (r.ccsOk ? String(r.eco2) : "") + ","
    + (r.ccsOk ? String(r.tvoc) : "") + ","
    + (r.htuOk ? String(r.tempC, 1) : "") + ","
    + (r.htuOk ? String(r.hum, 1) : "") + ","
    + (alarm ? "1" : "0");
  if (useSD) {
    File f = SD.open(SD_LOG_PATH, FILE_APPEND); if (f) { f.println(row); f.close(); }
  } else if (useFlash) {
    File f = LittleFS.open(FLASH_LOG_PATH, FILE_APPEND); if (f) { f.println(row); f.close(); }
    enforceFlashCap();
  }
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

  okHtu = htu.begin();               if (!okHtu) Serial.println("[sensors] HTU21D init failed");
  bootLine("HTU21D", okHtu ? "OK" : "FAIL");
  okCcs = ccs.begin(ADDR_CCS811);    if (!okCcs) Serial.println("[sensors] CCS811 init failed (check WAK->GND)");
  bootLine("CCS811", okCcs ? "OK" : "FAIL");

  spiSD.begin(PIN_SD_SCK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  useSD = SD.begin(PIN_SD_CS, spiSD, SD_HZ);
  if (useSD) {
    ensureHeader(SD, SD_LOG_PATH, LOG_HEADER);
    ensureHeader(SD, SD_NET_LOG, NET_LOG_HEADER);   // WiFi/BLE logs are SD-only
    ensureHeader(SD, SD_DEV_LOG, DEV_LOG_HEADER);
    Serial.println("[log] SD card mounted");
  }
  else {
    Serial.println("[log] no SD card; trying flash");
    useFlash = LittleFS.begin(true);
    if (useFlash) { ensureHeader(LittleFS, FLASH_LOG_PATH, LOG_HEADER); Serial.println("[log] logging to flash"); }
    else Serial.println("[log] flash init failed; not logging");
  }

  bootLine("Logging", useSD ? "SD" : (useFlash ? "Flash" : "none"));

  nfcProvision();
  bootLine("NFC",    "sent");

  WiFi.mode(WIFI_STA);              // station mode, not connected — scan only
  WiFi.disconnect(false);
  bootLine("WiFi", "OK");

  bleLock = xSemaphoreCreateMutex();
  NimBLEDevice::init("");
  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCb);          // advertisements -> bleUpsert()
  pScan->setActiveScan(true);                // active scan -> request device names
  pScan->setInterval(100);
  pScan->setWindow(99);
  pScan->setDuplicateFilter(false);          // keep seeing repeats to refresh RSSI/last-seen
  pScan->start(0 /*continuous*/, false);     // runs in the background, fills bleTab
  bootLine("BLE", "OK");

  historyInit();

  delay(1800);                 // hold the boot screen
  startMs = millis();
  nextTick = millis();
  lastR = readSensors();
  lastE = evaluate(lastR);
}

void loop() {
  unsigned long now = millis();
  updateBuzzer();
  handleButtons();
  serviceScans(now);

  if (now >= nextTick) {
    nextTick += SAMPLE_INTERVAL_MS;
    unsigned long uptimeS = (now - startMs) / 1000;
    animPhase++;                    // advance the "..." scanning animation
    bleSnapshot(now);              // refresh BLE device table + snapshot-log to SD

    lastR = readSensors();
    historyAdd(lastR, now);
    lastE = evaluate(lastR);

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
    logRow(lastR, uptimeS, lastE.harmful);
  }
  delay(5);
}
