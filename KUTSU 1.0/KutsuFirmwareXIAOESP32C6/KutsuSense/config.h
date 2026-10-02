#pragma once

// ===========================================================================
// KUTSU Sense — "pre-assembled" build config (Seeed XIAO ESP32-C6, Arduino)
// ---------------------------------------------------------------------------
// This is the NEW-HARDWARE branch: on-board Sensirion sensors (SCD41 CO2,
// SHT40 temp/RH, SGP41 VOC+NOx) + on-board OLED. The DIY branch (solder-on
// HTU21D + CCS811 modules, with NFC) lives in ../../KutsuXIAOESP32C3 and is
// left untouched. Pin/address facts here are taken from the board netlist
// (kutsu-board-xiao-esp32c5c6.net; XIAO symbol = U3).
//
// Pins use XIAO Dx labels (resolve to C6 GPIOs via the Seeed C6 variant).
// Raw C6 GPIOs: D0=0 D1=1 D2=2 D3=21 D4=22 D5=23 D6=16 D7=17 D8=19 D9=20 D10=18.
// Unlike the C3, D8/D9 (GPIO19/20) are NOT boot-strapping pins — SD is safe.
// ===========================================================================

#define PIN_SDA      D4        // net SDA  (U3 D4) — R10 pull-up to 3V3
#define PIN_SCL      D5        // net SCL  (U3 D5) — R11 pull-up to 3V3
#define I2C_HZ       100000

#define PIN_SD_SCK   D8        // net SDCLK (J1 CLK)
#define PIN_SD_MISO  D9        // net SDSO  (J1 DAT0)
#define PIN_SD_MOSI  D10       // net SDSI  (J1 CMD)
#define PIN_SD_CS    D3        // net SDCS  (J1 CD/DAT3)
#define SD_HZ        1000000

#define PIN_BUTTON   D6        // net BTN1 (SW1)
#define PIN_BUTTON2  D7        // net BTN2 (SW2, reserved)
#define PIN_BUZZER   D2        // net GPIO4 (BZ1 +)
#define PIN_LED      D1        // warning-indicator LED (hand-wired: D1 -> R -> LED -> GND)
#define LED_ACTIVE_HIGH 1      // 1 = drive HIGH to light the LED

// ---- XIAO ESP32-C6 RF switch / antenna (module-internal GPIOs, not Dx pads) ----
// The C6 routes its radio through an RF switch. GPIO3 powers the switch (drive
// LOW to enable); GPIO14 selects the antenna (LOW = on-board, HIGH = external
// u.FL). Required for reliable WiFi/BLE scanning. Verify against the Seeed
// XIAO ESP32-C6 wiki if the radio underperforms.
#define RF_SWITCH_EN_GPIO   3
#define RF_ANT_SEL_GPIO     14
#define RF_USE_EXTERNAL_ANT 0     // 0 = on-board ceramic antenna, 1 = external u.FL

// ---- I2C addresses (all fixed; no conflicts) ----
#define ADDR_SSD1306 0x3C     // U4 OLED (DM-OLED096-636)
#define ADDR_SHT40   0x44     // U2 SHT40-AD1B
#define ADDR_SGP41   0x59     // U1 SGP41
#define ADDR_SCD41   0x62     // U5 SCD41

#define OLED_W 128
#define OLED_H 64

#define SAMPLE_INTERVAL_MS 1000
#define CHART_BIN_MS       60000
#define CHART_BINS         60
#define ALARM_SHOW_MS      5000
#define ALARM_GAP_MS       1000

// SGP41: run NOx conditioning for the first N seconds (1 Hz) before normal
// measurement. VOC/NOx indices are "warming up" until then.
#define SGP41_CONDITION_S  10

// ---- Alarm thresholds (sev 1 = warn "!", sev 2 = alert "!!") ----
#define TEMP_HARM_LO   0.0f
#define TEMP_WARN_HI   50.0f
#define TEMP_HARM_HI   60.0f

#define HUM_HARM_LO    25.0f
#define HUM_WARN_LO    30.0f
#define HUM_WARN_HI    60.0f
#define HUM_HARM_HI    70.0f

// True NDIR CO2 (SCD41), ppm. Indoor guidance: ~1000 elevated, 2000 poor,
// 5000 = 8 h occupational limit.
#define CO2_WARN       1000
#define CO2_HARM       2000

// Sensirion VOC Index (1-500; 100 = recent baseline). PROVISIONAL — calibrate.
#define VOC_WARN       250
#define VOC_HARM       400

// Sensirion NOx Index (1-500; 1 = baseline). PROVISIONAL — calibrate.
#define NOX_WARN       20
#define NOX_HARM       150

// ---- WiFi / BLE monitoring (passive scanners, run in the background) ----
#define WIFI_SCAN_INTERVAL_MS  5000    // re-scan APs this often (async, background)
#define BLE_STALE_MS           30000   // drop a BLE device not seen for this long
#define BLE_TABLE_MAX          32      // max BLE devices tracked concurrently
#define NET_SHOW               6       // AP rows shown on the Network screen
#define BLE_SHOW               5       // device rows shown on the Nearby Devices screen
#define LOG_SCAN_INTERVAL_MS   60000   // append an AP + device snapshot to SD this often

// RSSI -> distance, log-distance path loss:  d(m) = 10 ^ ((ref1m - RSSI) / (10 * n))
// VERY approximate (indoors easily off 2-3x). Calibrate ref1m per build/environment.
#define WIFI_RSSI_REF_1M   -45.0f
#define BLE_RSSI_REF_1M    -59.0f
#define PATH_LOSS_N         2.7f

#define SD_LOG_PATH    "/kutsu_log.csv"
#define FLASH_LOG_PATH "/kutsu_log.csv"
#define FLASH_LOG_MAX  (256UL * 1024UL)
#define LOG_HEADER     "uptime_s,co2_ppm,voc_index,nox_index,temp_c,humidity_pct,alarm"

// WiFi/BLE scan logs — SD only (skipped when no card is present)
#define SD_NET_LOG     "/kutsu_net.csv"
#define SD_DEV_LOG     "/kutsu_dev.csv"
#define NET_LOG_HEADER "uptime_s,ssid,bssid,band,freq_mhz,channel,rssi,dist_m"
#define DEV_LOG_HEADER "uptime_s,mac,name,rssi,dist_m"
