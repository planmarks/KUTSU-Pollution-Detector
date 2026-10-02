/* ===========================================================================
 * KUTSU Sense — BLE GATT layer (ESP32 core "BLE" library, BLEDevice.h)
 * ===========================================================================
 * IMPORTANT: this uses the ESP32 Arduino core's BUNDLED BLE library, NOT the
 * external NimBLE-Arduino. On the ESP32-C5 the core BLE library is NimBLE-backed
 * (CONFIG_BT_NIMBLE_ENABLED=y, Bluedroid off) and — crucially — is compiled by
 * Espressif against the exact same C5 IDF BLE libs as the controller, so there
 * is no ABI mismatch. NimBLE-Arduino links but crashes at startup on the C5
 * (Load access fault) because its bundled host/porting layer conflicts with the
 * C5 controller libs; hence we do NOT use it here. Do not #include NimBLEDevice.h.
 *
 * API conventions follow the core's libraries/BLE/examples/Notify example:
 *   - notify characteristics get an addDescriptor(new BLE2902()) CCCD,
 *   - callbacks override the single-arg onWrite/onRead/onConnect (the NimBLE
 *     backend's *_conn_desc overloads forward to these).
 * =========================================================================== */
#include "ble_service.h"
#include "identity.h"
#include "kutsu_link.h"
#include "mesh.h"
#include "config.h"

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <ArduinoJson.h>

static BLECharacteristic* chrLive    = nullptr;
static BLECharacteristic* chrStatus  = nullptr;
static BLECharacteristic* chrHistory = nullptr;
static volatile bool      s_connected = false;

// ---- Status payload --------------------------------------------------------
static String statusJson() {
  JsonDocument d;
  d["name"]   = kutsuName();
  d["mac"]    = kutsuMacStr();
  d["role"]   = kutsuRoleStr(kutsuGetRole());
  d["ip"]     = kutsuMasterIp();          // own ip (master) / master ip (slave) / ""
  d["room"]   = kutsuRoom();
  d["synced"] = kutsuTimeSynced();
  d["ts"]     = kutsuEpochNow();
  d["v"]      = SCHEMA_VERSION;
  String s; serializeJson(d, s); return s;
}

// ---- Callbacks -------------------------------------------------------------
class ServerCb : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    s_connected = true;
    Serial.println("[ble] central connected");
  }
  void onDisconnect(BLEServer* s) override {
    s_connected = false;
    Serial.println("[ble] central disconnected; re-advertising");
    BLEDevice::startAdvertising();        // keep discoverable for the next app session
  }
};

// TIMEGEO write: {"epoch":1719000000,"lat":51.5,"lon":-0.12}  (req 2.2)
class TimeGeoCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    JsonDocument d;
    if (deserializeJson(d, c->getValue().c_str())) { Serial.println("[ble] timegeo parse fail"); return; }
    uint32_t epoch = d["epoch"] | 0UL;
    if (!epoch) return;
    bool haveGeo = d["lat"].is<double>() && d["lon"].is<double>();
    kutsuApplyTimeSync(epoch, haveGeo, d["lat"] | 0.0, d["lon"] | 0.0);
    Serial.printf("[ble] time sync epoch=%lu geo=%d\n", (unsigned long)epoch, haveGeo);
    bleNotifyStatus();
  }
};

// WIFI write: {"ssid":"..","password":"..","room":".."}
class WifiCb : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    JsonDocument d;
    if (deserializeJson(d, c->getValue().c_str())) { Serial.println("[ble] wifi parse fail"); return; }
    const char* ssid = d["ssid"] | "";
    if (!ssid[0]) return;
    netProvision(ssid, d["password"] | "", d["room"] | "");
    Serial.printf("[ble] wifi provisioned ssid=%s\n", ssid);
    bleNotifyStatus();
  }
};

// HISTORY read: refresh the value lazily with the current log tail (~2 KB).
class HistoryCb : public BLECharacteristicCallbacks {
  void onRead(BLECharacteristic* c) override {
    c->setValue(kutsuTailLog(2048));
  }
};

void bleNotifyStatus() {
  if (!chrStatus) return;
  chrStatus->setValue(statusJson());
  if (s_connected) chrStatus->notify();
}

// ---- Setup + pump ----------------------------------------------------------
static void bleInit() {
  BLEDevice::init(String(kutsuName()));
  BLEServer* server = BLEDevice::createServer();
  server->setCallbacks(new ServerCb());

  // 30 handles: 5 characteristics (two with a CCCD) + service comfortably fit.
  BLEService* svc = server->createService(BLEUUID(BLE_SVC_UUID), 30);

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
  chrStatus->setValue(statusJson());

  chrHistory = svc->createCharacteristic(
      BLE_CHR_HISTORY_UUID, BLECharacteristic::PROPERTY_READ);
  chrHistory->setCallbacks(new HistoryCb());
  chrHistory->setValue(String(""));

  svc->start();

  // Device Information Service (0x180A): expose the MAC as the serial number so
  // a generic scanner shows the unique id (req 2.4).
  BLEService* dis = server->createService("180A");
  dis->createCharacteristic("2A25", BLECharacteristic::PROPERTY_READ)->setValue(String(kutsuMacStr())); // serial number
  dis->createCharacteristic("2A29", BLECharacteristic::PROPERTY_READ)->setValue(String("Papathought")); // manufacturer
  dis->createCharacteristic("2A24", BLECharacteristic::PROPERTY_READ)->setValue(String("Kutsu Sense"));  // model
  dis->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(BLE_SVC_UUID);
  adv->setScanResponse(true);
  adv->setName(String(kutsuName()));
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as %s (%s)\n", kutsuName(), kutsuMacStr());
}

void bleTask(void* pv) {
  bleInit();
  uint32_t lastSeq = 0;
  for (;;) {
    uint32_t seq = kutsuSampleSeq();
    if (seq != lastSeq && (seq % BLE_NOTIFY_EVERY_N) == 0) {
      lastSeq = seq;
      if (chrLive) {
        chrLive->setValue(kutsuCurrentRecordJson());
        if (s_connected) chrLive->notify();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void bleStart() {
  // BLE + WiFi coexistence is memory-heavy on the single-core C5; give the task
  // an ample stack.
  xTaskCreate(bleTask, "bleTask", 8192, nullptr, 5, nullptr);
}
