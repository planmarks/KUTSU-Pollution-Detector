#pragma once

// ===========================================================================
// KUTSU Sense — KUTSU 1.0 config (Seeed XIAO ESP32 series, Arduino)
// ---------------------------------------------------------------------------
// ONE firmware for every XIAO that fits the Kutsu 1 board's footprint:
//   XIAO ESP32-C3 · XIAO ESP32-C5 · XIAO ESP32-C6 · XIAO ESP32-S3
// Pins are XIAO Dx labels; the selected board's Arduino variant resolves them
// to the right GPIOs (table below). Chip-specific bits (C6 RF switch, C5 dual
// band) are handled with #if in the sketch — nothing here needs editing when
// you swap modules.
//
//   pad   C3     C5     C6     S3     net on the Kutsu 1 board (rev 3)
//   D0    GPIO2  GPIO1  GPIO0  GPIO1  MQ2AO  (MQ-2 analog, via R2/R1 divider)
//   D1    GPIO3  GPIO0  GPIO1  GPIO2  D1     (warning LED, R15)
//   D2    GPIO4  GPIO25 GPIO2  GPIO3  GPIO4  (buzzer BZ1 +)
//   D3    GPIO5  GPIO7  GPIO21 GPIO4  SDCS
//   D4    GPIO6  GPIO23 GPIO22 GPIO5  SDA    (R13 + R4 pull-ups)
//   D5    GPIO7  GPIO24 GPIO23 GPIO6  SCL    (R14 + R3 pull-ups)
//   D6    GPIO21 GPIO11 GPIO16 GPIO43 BTN1   <- UART0 TX on every module
//   D7    GPIO20 GPIO12 GPIO17 GPIO44 BTN2   <- UART0 RX on every module
//   D8    GPIO8  GPIO8  GPIO19 GPIO7  SDCLK
//   D9    GPIO9  GPIO9  GPIO20 GPIO8  SDSO (MISO)
//   D10   GPIO10 GPIO10 GPIO18 GPIO9  SDSI (MOSI)
//
// Because D6/D7 are the UART0 pins on all four modules, "USB CDC On Boot" MUST
// be Enabled (the sketch refuses to compile otherwise) so Serial uses USB.
//
// SENSORS ARE AUTO-DETECTED. setup() scans the I2C bus and only initialises,
// displays, logs and charts what it finds. Supported parts:
//   on-board (KUTSU 1.0): SCD41 0x62 · SGP41 0x59 · SHT40 0x44/0x45
//   DIY breakouts .......: CCS811 0x5A/0x5B · HTU21D 0x40
//   display .............: SSD1306 128x64 at 0x3C/0x3D — the on-board OLED
//                          panel (DS1, DLC0096DNOG-W-5) or a plug-in module
//   analog ..............: MQ-2 module on D0 (optional; presence-checked)
// ===========================================================================

#define PIN_SDA      D4        // net SDA
#define PIN_SCL      D5        // net SCL
#define I2C_HZ       100000    // CCS811 clock-stretches; keep 100 kHz

#define PIN_SD_SCK   D8        // net SDCLK (J1 CLK)
#define PIN_SD_MISO  D9        // net SDSO  (J1 DAT0)
#define PIN_SD_MOSI  D10       // net SDSI  (J1 CMD)
#define PIN_SD_CS    D3        // net SDCS  (J1 CD/DAT3)
#define SD_HZ        1000000

#define PIN_BUTTON   D6        // net BTN1 (SW2 on rev 3)
#define PIN_BUTTON2  D7        // net BTN2 (SW3 on rev 3, reserved)
#define PIN_BUZZER   D2        // net GPIO4 (BZ1 +)
#define PIN_LED      D1        // warning LED (D1 -> R15 -> LED -> GND)
#define LED_ACTIVE_HIGH 1      // 1 = drive HIGH to light the LED

// Buzzer drive. BZ1 on the Kutsu 1 board (PUI SMT-1028-T-2-R) is a MAGNETIC
// TRANSDUCER (18 ohm coil, resonance ~2.7-2.8 kHz): a DC level only clicks, it
// must be driven with a square wave. DIY builds with a self-oscillating ACTIVE
// buzzer can use either setting; 0 = plain on/off (DC) drive.
#define BUZZER_TONE_HZ   2700  // 0 = active buzzer (DC); >0 = PWM tone frequency

// ---- XIAO ESP32-C6 RF switch (module-internal GPIOs; C6 only) ---------------
// The board variant already powers the switch (GPIO3 LOW) and selects the
// on-board antenna (GPIO14 LOW) before setup(). Set 1 for the enclosure's
// external whip antenna on the u.FL connector.
#define RF_SWITCH_EN_GPIO   3
#define RF_ANT_SEL_GPIO     14
#define RF_USE_EXTERNAL_ANT 0     // 0 = on-board ceramic antenna, 1 = external u.FL

// ---- I2C addresses (probed at boot; first match wins) -----------------------
#define ADDR_SSD1306     0x3C   // DS1 panel: D/C# (SA0) tied to GND -> 0x3C
#define ADDR_SSD1306_ALT 0x3D   // modules with the address jumper moved
#define ADDR_SCD41       0x62   // U3 SCD41
#define ADDR_SGP41       0x59   // U1 SGP41 (fixed)
#define ADDR_SHT40       0x44   // U2 SHT40-AD1B
#define ADDR_SHT40_ALT   0x45   // SHT40-BD1B variant
#define ADDR_HTU21D      0x40   // DIY HTU21D breakout (fixed)
#define ADDR_CCS811      0x5A   // DIY CCS811 breakout, ADDR -> GND
#define ADDR_CCS811_ALT  0x5B   // CCS811 with ADDR -> VCC

// ---- Display ------------------------------------------------------------------
#define OLED_W 128
#define OLED_H 64
// 0 = normal, 2 = rotated 180 deg. The on-board panel and a plug-in module can
// sit in opposite orientations depending on how they are mounted; flip here.
#define DISPLAY_ROTATION 0

#define SAMPLE_INTERVAL_MS 1000
#define CHART_BIN_MS       60000
#define CHART_BINS         60
#define ALARM_SHOW_MS      5000
#define ALARM_GAP_MS       1000
#define NOTICE_MS          4000   // "Hub connected" banner duration

// SGP41: run NOx conditioning for the first N seconds (1 Hz) before normal
// measurement. VOC/NOx indices read "--" until then.
#define SGP41_CONDITION_S  10
// Diagnostic: print the raw SGP41 signals each second so you can see whether the
// NOx raw moves when exposed to a NOx source (gas flame). Set 0 once resolved.
#define SGP41_DEBUG        1

// ---- MQ-2 gas/smoke module on D0 (optional) -----------------------------------
// AO -> R2 (10k) -> D0, D0 -> R1 (20k) -> GND: the 0-5 V output is scaled to
// 0-3.3 V. Raw 12-bit ADC count, NOT calibrated ppm. With no module (or on
// battery, where the 5 V rail is absent) R1 holds D0 at ~0, so a reading below
// GAS_PRESENT_MIN means "not fitted" and the metric stays hidden until a valid
// reading appears.
#define ENABLE_MQ2        1
#define PIN_MQ2_AO        D0
#define GAS_SAMPLES       8      // ADC samples averaged per reading
#define GAS_PRESENT_MIN   40     // raw ADC below this => module absent/unpowered
#define GAS_WARN          1800   // raw 0-4095 — CALIBRATE
#define GAS_HARM          2600   // raw 0-4095 — CALIBRATE

// ---- Alarm thresholds (sev 1 = warn "!", sev 2 = alert "!!") ---------------
#define TEMP_HARM_LO   0.0f
#define TEMP_WARN_HI   50.0f
#define TEMP_HARM_HI   60.0f

#define HUM_HARM_LO    25.0f
#define HUM_WARN_LO    30.0f
#define HUM_WARN_HI    60.0f
#define HUM_HARM_HI    70.0f

// True NDIR CO2 (SCD41), ppm. ~1000 elevated, 2000 poor, 5000 = 8 h limit.
#define CO2_WARN       1000
#define CO2_HARM       2000

// Estimated eCO2 / TVOC (CCS811 breakout).
#define ECO2_WARN      900
#define ECO2_HARM      2000
#define TVOC_WARN      660     // ppb
#define TVOC_HARM      3000

// Sensirion VOC Index (1-500; 100 = recent baseline). PROVISIONAL — calibrate.
#define VOC_WARN       250
#define VOC_HARM       400

// Sensirion NOx Index (1-500; 1 = baseline). PROVISIONAL — calibrate.
#define NOX_WARN       20
#define NOX_HARM       150

// ---- WiFi / BLE monitoring (passive scanners, run in the background) ----------
#define WIFI_SCAN_INTERVAL_MS        5000   // offline (handheld): re-scan APs this often
#define WIFI_SCAN_INTERVAL_ONLINE_MS 60000  // online: scans pause traffic, so rarely
#define BLE_STALE_MS           30000   // drop a BLE device not seen for this long
#define BLE_SCAN_INTERVAL_MS   100     // BLE scan period ...
#define BLE_SCAN_WINDOW_MS     50      // ... of which BLE listens this long (50% duty).
                                       // Every XIAO has ONE radio shared by WiFi + BLE;
                                       // ~100% BLE duty starves WiFi and has caused
                                       // watchdog resets on the C3.
#define BLE_TABLE_MAX          32      // max BLE devices tracked concurrently
#define NET_SHOW               6       // AP rows shown on the Networks screen
#define BLE_SHOW               5       // device rows shown on the Devices screen
#define LOG_SCAN_INTERVAL_MS   60000   // append an AP + device snapshot to SD this often

// RSSI -> distance, log-distance path loss:  d(m) = 10 ^ ((ref1m - RSSI) / (10 * n))
// VERY approximate (indoors easily off 2-3x). Calibrate ref1m per build/environment.
#define WIFI_RSSI_REF_1M   -45.0f
#define BLE_RSSI_REF_1M    -59.0f
#define PATH_LOSS_N         2.7f

// ---- Logging (JSON Lines — see DATA_CONTRACT.md) ------------------------------
// One JSON record per line; the same string the dashboard/mesh transmits.
// MicroSD if a card is present at boot, else internal flash (LittleFS).
#define FIRMWARE_VERSION    "0.10"    // human-readable firmware release (reported in STATUS as "fw")
#define SCHEMA_VERSION      4        // v4: re-adds lat/lon/loc_ts + phone/BLE/USB time+geo source
#define SD_LOG_PATH         "/kutsu_log.jsonl"
#define FLASH_LOG_PATH      "/kutsu_log.jsonl"
#define FLASH_LOG_OLD_PATH  "/kutsu_log.1.jsonl"   // previous file after rotation
#define FLASH_LOG_MAX       (384UL * 1024UL)       // rotate at this size (2 files kept)
#define FLASH_LOG_EVERY_N   10                     // flash: keep 1 sample in N (wear + space)

// WiFi/BLE scan logs — SD only (skipped when no card is present)
#define SD_NET_LOG     "/kutsu_net.csv"
#define SD_DEV_LOG     "/kutsu_dev.csv"
#define NET_LOG_HEADER "uptime_s,ssid,bssid,band,freq_mhz,channel,rssi,dist_m"
#define DEV_LOG_HEADER "uptime_s,mac,name,rssi,dist_m"

// ===========================================================================
// WIFI + KUTSU HUB (stationary mode)
// ---------------------------------------------------------------------------
// In STATIONARY mode the unit joins WiFi and sends its data to the KUTSU Hub
// (../../KutsuHub — a collector + dashboard app on a Raspberry Pi or PC on the
// same LAN). No credentials, or the network can't be reached -> the unit keeps
// working stand-alone (display + alarm + SD/flash log + passive scans) and
// retries in the background every WIFI_RETRY_MS. HANDHELD mode never joins WiFi.
//
// Credentials: set them here, or at runtime over USB serial (115200):
//   ssid <name> / pass <password> / room <label> / connect / forget / status
// Runtime values are stored in NVS and take precedence over the ones below.
// ===========================================================================
#define WIFI_SSID           ""        // "" = no default network (handheld only)
#define WIFI_PASSWORD       ""
#define ROOM_LABEL          ""        // dashboard group label, e.g. "Workshop"

#define NVS_NAMESPACE       "kutsu"   // Preferences namespace (ssid/pass/room)
#define WIFI_CONNECT_MS     15000     // max wait for an association attempt
#define WIFI_RETRY_MS       60000     // offline: retry the network this often (0 = never)

// Real time over NTP once online (log `ts` stays null until then).
#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.google.com"

// ===========================================================================
// BLE PROVISIONING (GATT server — the "Kutsu Environmental Service")
// ---------------------------------------------------------------------------
// Lets the Kutsu desktop app set up the device over Bluetooth LE (WiFi creds +
// room, live status) with no USB cable — see ble_provisioning.cpp and the
// shared contract in ../../PROVISIONING.md. These UUIDs are identical to the
// KUTSU Stationary firmware's, so one app speaks to every build. This is a
// GATT *server* layered on top of the passive BLE scanner; set PROVISION_BLE 0
// if server+scanner coexistence misbehaves on a given module (e.g. the C3).
#define PROVISION_BLE        1        // advertise the provisioning service. (Boot-crash fix: the
                                      // sketch calls BLEDevice::init("") — a non-empty name there
                                      // asserts in NimBLE ble_svc_gap_init on the C5/C6. Set to 0
                                      // to drop the BLE server entirely if a module still misbehaves.)
#define BLE_BONDING          0        // (#1) advertise LE "Just Works" bonding so a phone can PAIR with
                                      // the device — the OS then remembers it and reconnects on its own.
                                      // No passkey UI; characteristics stay readable without encryption,
                                      // so non-bonding clients (nRF Connect) still work. DEFAULT 0: the
                                      // BLESecurity API uses Bluedroid enums that may not compile on a
                                      // NimBLE-backed core (C5/C6). Set 1 and verify a clean build on your
                                      // board to enable pairing. The app's own auto-reconnect works either
                                      // way, so this is extra hardening, not required.
#define BLE_SVC_UUID         "6b755473-7500-4b75-7473-750000000001"  // service
#define BLE_CHR_LIVE_UUID    "6b755473-7500-4b75-7473-750000000002"  // read/notify: record JSON
#define BLE_CHR_TIMEGEO_UUID "6b755473-7500-4b75-7473-750000000003"  // write : {epoch[,lat,lon]}
#define BLE_CHR_WIFI_UUID    "6b755473-7500-4b75-7473-750000000004"  // write : {ssid,password[,room]}
#define BLE_CHR_STATUS_UUID  "6b755473-7500-4b75-7473-750000000005"  // read/notify: {name,mac,role,ip,...}
#define BLE_CHR_HISTORY_UUID "6b755473-7500-4b75-7473-750000000006"  // (reserved; not served on this build)
#define BLE_CHR_SETTINGS_UUID "6b755473-7500-4b75-7473-750000000007" // read/write/notify: {name,buzzer,led,display,scan,mode,buzzer_sig,led_warn,led_alert}
#define BLE_CHR_THRESH_UUID   "6b755473-7500-4b75-7473-750000000008" // read/write: per-metric warn/harm thresholds (4.1)
#define BLE_NOTIFY_EVERY_N   1        // notify the LIVE characteristic every N samples (0 = never)

// ---- KUTSU Hub client -----------------------------------------------------------
// The hub is found automatically (UDP broadcast) unless an address is pinned here
// or over serial (`hub 192.168.1.20:8080` / `hub auto`); the NVS value wins.
// The UDP port must match the hub's --udp-port.
#define HUB_ADDRESS         ""        // "" = auto-discover; or "192.168.1.20:8080"
#define HUB_DEFAULT_PORT    8080      // used when an address/beacon gives no port
#define HUB_UDP_PORT        47800     // discovery probe / beacon port
#define HUB_PUSH_MS         5000      // live reading + new log lines every N ms
#define HUB_TIMEOUT_MS      30000     // hub silent this long -> rediscover (auto mode)
#define HUB_NTP_GRACE_MS    10000     // no NTP answer this long after joining -> use the hub's clock
#define HUB_BATCH_BYTES     4096      // log bytes per POST (whole lines)
#define HUB_MAX_BATCHES     16        // batches per push interval (backlog catch-up speed)
