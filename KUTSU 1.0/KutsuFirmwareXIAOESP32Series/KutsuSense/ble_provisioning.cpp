/* ===========================================================================
 * KUTSU Sense — BLE provisioning service (GATT server) — implementation
 * ===========================================================================
 * Uses the ESP32 Arduino core's BUNDLED BLE library (BLEDevice.h), NOT the
 * external NimBLE-Arduino (which crashes on the C5/C6 — the core library is the
 * one compiled against the matching IDF BLE libs). Mirrors the KUTSU Stationary
 * ble_service.cpp, but adapted to the XIAO-series core link (kutsu_link.h):
 *   - does NOT call BLEDevice::init()  (the scanner already did — see the .h),
 *   - TIMEGEO applies epoch only       (this build's kutsuApplyTimeSync has no geo),
 *   - HISTORY (…0006) is omitted here  (no log-tail accessor on this build yet).
 * The WIFI + STATUS + LIVE characteristics — everything the app needs to set up
 * and monitor the device — are identical to the Stationary contract.
 * =========================================================================== */
#include "ble_provisioning.h"
#include "config.h"

#if PROVISION_BLE

#include "identity.h"
#include "kutsu_link.h"
#include "mesh.h"
#include "settings.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#if BLE_BONDING
#include <BLESecurity.h>
#endif
#include <ArduinoJson.h>

static BLECharacteristic* chrLive     = nullptr;
static BLECharacteristic* chrStatus   = nullptr;
static BLECharacteristic* chrSettings = nullptr;
static volatile bool      s_connected = false;

// ---- Status payload: {name,mac,role,ip,room,synced,ts,v} -------------------
static String statusJson() {
  JsonDocument d;
  d["name"]   = kutsuName();
  d["mac"]    = kutsuMacStr();
  d["role"]   = kutsuRoleStr(kutsuGetRole());
  d["ip"]     = netIp();                 // own IP while online, else ""
  d["hub"]    = netHub();                // "ip:port" of the KUTSU Hub being fed (dashboard URL), else ""
  d["room"]   = kutsuRoom();
  d["synced"] = kutsuTimeSynced();
  d["ts"]     = kutsuEpochNow();         // 0 until NTP/app sync
  d["geo"]    = kutsuHaveGeo();          // whether a location fix has been supplied
  if (kutsuHaveGeo()) { d["lat"] = kutsuLat(); d["lon"] = kutsuLon(); }
  d["fw"]     = FIRMWARE_VERSION;
  d["v"]      = SCHEMA_VERSION;
  String s; serializeJson(d, s); return s;
}

void bleProvisioningNotifyStatus() {
  if (!chrStatus) return;
  chrStatus->setValue(statusJson());
  if (s_connected) chrStatus->notify();
}

// ---- Callbacks -------------------------------------------------------------
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer*) override {
    s_connected = true;
    Serial.println("[ble] central connected");
  }
  void onDisconnect(BLEServer*) override {
    s_connected = false;
    Serial.println("[ble] central disconnected; re-advertising");
    BLEDevice::startAdvertising();       // stay discoverable for the next app session
  }
};

// WIFI write: {"ssid":"..","password":"..","room":".."}  (room optional)
class WifiCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    JsonDocument d;
    if (deserializeJson(d, c->getValue().c_str())) { Serial.println("[ble] wifi parse fail"); return; }
    const char* ssid = d["ssid"] | "";
    if (!ssid[0]) return;                 // empty ssid over BLE is ignored (use the app's "Forget")
    const char* room = d["room"] | "";
    netProvision(ssid, d["password"] | "", room[0] ? room : nullptr);
    Serial.printf("[ble] wifi provisioned ssid=%s\n", ssid);
    bleProvisioningNotifyStatus();
  }
};

// STATUS read: refresh lazily so a polling app always sees the live role/IP/time.
class StatusCb : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c) override { c->setValue(statusJson()); }
};

// SETTINGS read/write: {name,buzzer,led,display,scan,mode,buzzer_sig,led_warn,led_alert}.
// Read returns live state; write applies any subset and echoes the new state back.
class SettingsCb : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c) override { c->setValue(settingsJson()); }
  void onWrite(BLECharacteristic* c) override {
    if (settingsApplyJson(c->getValue().c_str())) Serial.println("[ble] settings updated");
    String s = settingsJson(); c->setValue(s);
    if (s_connected) c->notify();
  }
};

// THRESHOLDS read/write (4.1): per-metric warn/harm. Write applies any subset;
// {"reset":true} restores the config.h defaults.
class ThreshCb : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c) override { c->setValue(settingsThresholdsJson()); }
  void onWrite(BLECharacteristic* c) override {
    if (settingsApplyThresholdsJson(c->getValue().c_str())) Serial.println("[ble] thresholds updated");
    c->setValue(settingsThresholdsJson());
  }
};

// TIMEGEO write: {"epoch":1719000000,"lat":..,"lon":..}. Epoch and geo are applied
// independently (a phone may send only one), and the record then carries both.
class TimeGeoCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    JsonDocument d;
    if (deserializeJson(d, c->getValue().c_str())) { Serial.println("[ble] timegeo parse fail"); return; }
    uint32_t epoch = d["epoch"] | 0UL;
    if (epoch > 1700000000UL) kutsuApplyTimeSync(epoch, "app");
    if (d["lat"].is<double>() && d["lon"].is<double>())
      kutsuApplyGeo(d["lat"].as<double>(), d["lon"].as<double>());
    bleProvisioningNotifyStatus();
  }
};

// HISTORY read-back (#3): stream the stored SD/flash JSON-Lines log to the app.
// Protocol (MTU-agnostic — the value is always set by the WRITE, so a client's
// Read Long reassembles a stable value with no server-side side effects):
//   write {"cmd":"size"}  -> value becomes {"size":N}  (N = total log bytes)
//   write {"o":<offset>}   -> value becomes up to HIST_CHUNK raw bytes at offset
// The app queries size, then pulls offset 0..N, concatenates, and splits on '\n'
// into JSON records. A 0-byte chunk means end/empty.
static const size_t HIST_CHUNK = 400;
class HistoryCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    JsonDocument d;
    if (deserializeJson(d, c->getValue().c_str())) return;
    if (d["cmd"].is<const char*>() && String(d["cmd"].as<const char*>()) == "size") {
      JsonDocument r; r["size"] = (uint32_t)kutsuLogByteSize();
      String s; serializeJson(r, s); c->setValue(s);
      return;
    }
    if (d["o"].is<int>() || d["o"].is<unsigned int>()) {
      size_t off = (size_t)d["o"].as<uint32_t>();
      static uint8_t buf[HIST_CHUNK];
      size_t n = kutsuLogReadChunk(off, (char*)buf, HIST_CHUNK);
      c->setValue(buf, n);   // raw bytes; n == 0 at/after EOF
    }
  }
};

static void provTask(void*) {
  uint32_t lastSeq = 0;
  for (;;) {
    uint32_t seq = kutsuSampleSeq();
    if (seq != lastSeq && (BLE_NOTIFY_EVERY_N == 0 || (seq % BLE_NOTIFY_EVERY_N) == 0)) {
      lastSeq = seq;
      if (chrLive) {
        chrLive->setValue(kutsuCurrentRecordJson());
        if (s_connected) chrLive->notify();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void bleProvisioningStart() {
  // BLEDevice::init() was already called by the scanner setup (with the device
  // name). Build the GATT server on top of it.
#if BLE_BONDING
  // (#1) Enable LE "Just Works" bonding so a central (the phone) can pair and the
  // OS then auto-reconnects. No MITM / no passkey UI. Characteristic permissions
  // are left open (no encryption required), so non-bonding clients still work.
  BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
  {
    BLESecurity* sec = new BLESecurity();
    sec->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    sec->setCapability(ESP_IO_CAP_NONE);
    sec->setInitEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
    sec->setRespEncryptionKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  }
#endif
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  BLEService* svc = server->createService(BLEUUID(BLE_SVC_UUID), 40);  // 7 chars, 3 with a CCCD

  chrLive = svc->createCharacteristic(
      BLE_CHR_LIVE_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chrLive->addDescriptor(new BLE2902());
  chrLive->setValue(String("{}"));

  BLECharacteristic* chrTime = svc->createCharacteristic(
      BLE_CHR_TIMEGEO_UUID, BLECharacteristic::PROPERTY_WRITE);
  chrTime->setCallbacks(new TimeGeoCb());

  BLECharacteristic* chrWifi = svc->createCharacteristic(
      BLE_CHR_WIFI_UUID, BLECharacteristic::PROPERTY_WRITE);
  chrWifi->setCallbacks(new WifiCb());

  chrStatus = svc->createCharacteristic(
      BLE_CHR_STATUS_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  chrStatus->addDescriptor(new BLE2902());
  chrStatus->setCallbacks(new StatusCb());
  chrStatus->setValue(statusJson());

  chrSettings = svc->createCharacteristic(
      BLE_CHR_SETTINGS_UUID,
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_NOTIFY);
  chrSettings->addDescriptor(new BLE2902());
  chrSettings->setCallbacks(new SettingsCb());
  chrSettings->setValue(settingsJson());

  BLECharacteristic* chrThresh = svc->createCharacteristic(
      BLE_CHR_THRESH_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  chrThresh->setCallbacks(new ThreshCb());
  chrThresh->setValue(settingsThresholdsJson());

  // HISTORY (#3): read back the stored log for SD import over BLE.
  BLECharacteristic* chrHistory = svc->createCharacteristic(
      BLE_CHR_HISTORY_UUID, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE);
  chrHistory->setCallbacks(new HistoryCb());
  chrHistory->setValue(String("{}"));

  svc->start();

  // Device Information Service (0x180A): expose the MAC as the serial number so a
  // generic scanner shows the unique id even before connecting.
  BLEService* dis = server->createService("180A");
  dis->createCharacteristic("2A25", BLECharacteristic::PROPERTY_READ)->setValue(String(kutsuMacStr()));
  dis->createCharacteristic("2A29", BLECharacteristic::PROPERTY_READ)->setValue(String("Papathought"));
  dis->createCharacteristic("2A24", BLECharacteristic::PROPERTY_READ)->setValue(String("Kutsu Sense"));
  dis->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  adv->setName(String(kutsuName()));
  BLEDevice::startAdvertising();
  Serial.printf("[ble] provisioning service advertising as %s (%s)\n", kutsuName(), kutsuMacStr());

  xTaskCreate(provTask, "bleProv", 8192, nullptr, 4, nullptr);   // JSON build + BLE needs headroom
}

#else  // !PROVISION_BLE
void bleProvisioningStart() {}
void bleProvisioningNotifyStatus() {}
#endif
