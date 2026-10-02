#pragma once

/* ===========================================================================
 * KUTSU Sense — config for the Espressif ESP32-C5-DevKitC-1 (Arduino)
 * ---------------------------------------------------------------------------
 * Ported from the Seeed XIAO ESP32-C5 build (../KutsuXIAOESP32C5). The XIAO
 * used board D-macros (D0..D10); the DevKitC-1 exposes RAW GPIO numbers, so all
 * pins below are plain GPIO integers.
 *
 * Pins were picked from the ESP32-C5 datasheet v1.3 + the DevKitC-1 header list.
 * DO NOT use these (they break boot / flash / USB / console):
 *   - Strapping pins ....... GPIO2, 3, 7, 25, 26, 27, 28
 *   - Off-chip flash/PSRAM .. GPIO15-18, 20-22
 *   - USB-Serial-JTAG ....... GPIO13, 14   (USB-C: flashing + Serial Monitor)
 *   - UART0 console ......... GPIO11, 12
 * Two signals sit on strapping pins by necessity, each chosen so the strap it
 * touches is harmless here: the YELLOW LED on GPIO26 (weak output load) and the
 * MQ-2 analog input on GPIO3 (its strap only sets the unused SDIO clock edge,
 * and GPIO3 = ADC1_CH2 is the only free ADC pin). See WIRING.md. ============= */

// ---- Device identity (used in the JSON log / data contract) --------------
// DEVICE_ID is now only a compile-time fallback: the runtime id/name is
// MAC-derived ("Kutsu-A1B2C3") — see identity.h / kutsuName(). The data
// contract carries the runtime "id" field; keep this macro for builds/tests
// that run before identityInit().
#define DEVICE_ID       "kutsu-01"   // fallback only; real id = kutsuName()
#define SCHEMA_VERSION  2            // v2 adds id/mac/role/room/ts/lat/lon (see DATA_CONTRACT.md)

// ---- I2C bus (HTU21D, OLED, NTAG, CCS811) --------------------------------
// The C5 has no onboard I2C pull-ups: add external 4.7k from SDA->3V3 and
// SCL->3V3. (SDA=GPIO6 is the board's silkscreen SDA; SCL uses GPIO8 instead of
// the silkscreen SCL=GPIO7 because GPIO7 is a strapping pin.)
#define PIN_SDA      6
#define PIN_SCL      8
#define I2C_HZ       100000

// ---- MicroSD (SPI) -------------------------------------------------------
#define PIN_SD_SCK   23
#define PIN_SD_MISO  24
#define PIN_SD_MOSI  9
#define PIN_SD_CS    10
#define SD_HZ        1000000

// ---- Buttons & buzzer ----------------------------------------------------
#define PIN_BUTTON   1     // screen button: leg -> GPIO1, other leg -> GND (INPUT_PULLUP, active-low)
#define PIN_BUTTON2  25    // RESERVED, leave UNWIRED — GPIO25 is a strapping pin (unused in firmware)
#define PIN_BUZZER   4     // active buzzer + -> GPIO4, - -> GND

// ---- MQ-2 gas & smoke sensor (analog) ------------------------------------
// AO -> 10k -> GPIO3, and GPIO3 -> 10k -> GND  (÷2 divider: AO 0-5V -> pin 0-2.5V).
// GPIO3 = ADC1_CH2. Reading is a RAW 12-bit ADC count (0-4095), NOT calibrated
// ppm — MQ-2 needs a load-resistor + clean-air calibration for real ppm, which
// is why this build reports a relative level. CALIBRATE the two thresholds below
// against your own smoke/gas test. Presence check: an unpowered/unplugged sensor
// leaves the pin pulled to GND by the divider (~0), so a reading below
// GAS_PRESENT_MIN is treated as "sensor absent" and shown as "--".
#define PIN_MQ2_AO       3
#define GAS_SAMPLES      8      // ADC samples averaged per reading (noise smoothing)
#define GAS_PRESENT_MIN  40     // raw ADC below this => sensor absent/unpowered
#define GAS_WARN         1800   // raw ADC 0-4095 — CALIBRATE (warning "!" level)
#define GAS_HARM         2600   // raw ADC 0-4095 — CALIBRATE (alert level)

// ---- Status indicator LEDs -----------------------------------------------
// Each LED: GPIO -> ~330 ohm -> LED anode, LED cathode -> GND (active-HIGH).
// Behaviour (see updateLeds() in the .ino):
//   GOOD  (all readings good) ....... GREEN solid
//   WARN  (a few "!" warnings) ...... GREEN + YELLOW solid
//   WARN-MANY (>3 metrics warned) ... YELLOW solid only (green off — conditions suboptimal)
//   ALERT (any alert) ............... RED blinking
//   ERROR (a sensor failed to init) . YELLOW blinking
// PIN_LED_YELLOW is on strapping GPIO26 (weak output load can't disturb boot).
#define PIN_LED_GREEN   5
#define PIN_LED_YELLOW  26
#define PIN_LED_RED     0
#define LED_BLINK_MS    250    // blink half-period for the ALERT/ERROR states (ms)
#define WARN_MANY_COUNT 3      // MORE than this many metrics warned at once => green off, yellow only

// ---- I2C addresses -------------------------------------------------------
#define ADDR_SSD1306 0x3C
#define ADDR_HTU21D  0x40
#define ADDR_NTAG    0x55
#define ADDR_CCS811  0x5A

#define OLED_W 128
#define OLED_H 64

#define SAMPLE_INTERVAL_MS 1000
#define CHART_BIN_MS       60000
#define CHART_BINS         60
#define ALARM_SHOW_MS      5000
#define ALARM_GAP_MS       1000

#define TEMP_HARM_LO   0.0f
#define TEMP_WARN_HI   50.0f
#define TEMP_HARM_HI   60.0f

#define HUM_HARM_LO    25.0f
#define HUM_WARN_LO    30.0f
#define HUM_WARN_HI    60.0f
#define HUM_HARM_HI    70.0f

#define ECO2_WARN      900
#define ECO2_HARM      2000

#define TVOC_WARN      660
#define TVOC_HARM      3000

// ---- Logging -------------------------------------------------------------
// One JSON object per line (JSON Lines / NDJSON) so a Wi-Fi web dashboard or a
// BLE mobile app can consume each record directly — the exact same string the
// firmware writes to SD is what the future radio layer will transmit. Schema is
// documented in DATA_CONTRACT.md. SD is primary; internal flash (LittleFS) is
// the fallback when no card is present.
#define SD_LOG_PATH    "/kutsu_log.jsonl"
#define FLASH_LOG_PATH "/kutsu_log.jsonl"
#define FLASH_LOG_MAX  (256UL * 1024UL)

#define NFC_FULL_NAME  "Jane Doe"
#define NFC_DOB        "1990-01-01"
#define NFC_BLOOD      "O+"

// ===========================================================================
// CONNECTIVITY (BLE + WiFi mesh) — added for the app / dashboard roadmap
// ===========================================================================

// ---- BLE GATT (NimBLE) ----------------------------------------------------
// One custom "Kutsu Environmental Service" carrying live data + the phone's
// time/geo + WiFi provisioning. UUIDs are random-but-fixed for this product;
// the app must match them. (128-bit base kept human-scannable: ...-KUTSU.)
#define BLE_SVC_UUID         "6b755473-7500-4b75-7473-750000000001"  // service
#define BLE_CHR_LIVE_UUID    "6b755473-7500-4b75-7473-750000000002"  // notify: record JSON
#define BLE_CHR_TIMEGEO_UUID "6b755473-7500-4b75-7473-750000000003"  // write : {epoch,lat,lon}
#define BLE_CHR_WIFI_UUID    "6b755473-7500-4b75-7473-750000000004"  // write : {ssid,password,room}
#define BLE_CHR_STATUS_UUID  "6b755473-7500-4b75-7473-750000000005"  // read/notify: {name,mac,role,ip,...}
#define BLE_CHR_HISTORY_UUID "6b755473-7500-4b75-7473-750000000006"  // read/indicate: recent log lines
#define BLE_NOTIFY_EVERY_N  1        // notify the live characteristic every N samples

// ---- WiFi provisioning / persistence --------------------------------------
#define NVS_NAMESPACE       "kutsu"  // Preferences namespace (ssid/pass/room/bootepoch)
#define WIFI_CONNECT_MS     15000    // max wait for a WiFi association before giving up

// ---- Node mesh (mDNS discovery + master election) -------------------------
// First Kutsu on the LAN becomes MASTER and serves the dashboard; later nodes
// discover it and push their samples to it. See mesh.cpp.
#define MDNS_SERVICE        "kutsu"  // advertised as _kutsu._tcp
#define MDNS_PROTO          "tcp"
#define WEB_PORT            80        // master dashboard + ingest API
#define MESH_DISCOVER_MS    3000      // mDNS browse window before deciding master/slave
#define MESH_HEARTBEAT_MS   5000      // slave->master push / master-liveness cadence
#define MESH_MASTER_TIMEOUT 15000     // slave: master silent this long => re-elect
#define MESH_MAX_NODES      16        // master: max rooms/nodes tracked in RAM
