#pragma once

// Board: Seeed XIAO ESP32-C3 (Arduino). Pins use XIAO Dx labels, identical to
// the C5 build. Raw GPIOs: D2=4 D3=5 D4=6 D5=7 D6=21 D7=20 D8=8 D9=9 D10=10.
// NOTE: D8 (GPIO8) and D9 (GPIO9) are C3 boot-strapping pins — a MicroSD wired
// to SCK/MISO can block normal boot; see README §5 if the board won't run.

#define PIN_SDA      D4
#define PIN_SCL      D5
#define I2C_HZ       100000

#define PIN_SD_SCK   D8
#define PIN_SD_MISO  D9
#define PIN_SD_MOSI  D10
#define PIN_SD_CS    D3
#define SD_HZ        1000000

#define PIN_BUTTON   D6
#define PIN_BUTTON2  D7
#define PIN_BUZZER   D2

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

// ---- WiFi / BLE monitoring (passive scanners, run in the background) ----
#define WIFI_SCAN_INTERVAL_MS  5000    // re-scan APs this often (async, background)
#define BLE_STALE_MS           30000   // drop a BLE device not seen for this long
#define BLE_TABLE_MAX          32      // max BLE devices tracked concurrently
#define NET_SHOW               6       // AP rows shown on the Network screen (strongest first)
#define BLE_SHOW               5       // device rows shown on the Nearby Devices screen
#define LOG_SCAN_INTERVAL_MS   60000   // append an AP + device snapshot to SD this often

// RSSI -> distance, log-distance path loss:  d(m) = 10 ^ ((ref1m - RSSI) / (10 * n))
// VERY approximate (indoors easily off 2-3x). Calibrate ref1m per build/environment.
#define WIFI_RSSI_REF_1M   -45.0f      // typical AP RSSI at 1 m
#define BLE_RSSI_REF_1M    -59.0f      // typical BLE advertiser RSSI at 1 m
#define PATH_LOSS_N         2.7f       // 2.0 free space, ~2.7-4.0 indoors

#define SD_LOG_PATH    "/kutsu_log.csv"
#define FLASH_LOG_PATH "/kutsu_log.csv"
#define FLASH_LOG_MAX  (256UL * 1024UL)
#define LOG_HEADER     "uptime_s,eco2_ppm,tvoc_ppb,temp_c,humidity_pct,alarm"

// WiFi/BLE scan logs — SD only (skipped when no card is present)
#define SD_NET_LOG     "/kutsu_net.csv"
#define SD_DEV_LOG     "/kutsu_dev.csv"
#define NET_LOG_HEADER "uptime_s,ssid,bssid,band,freq_mhz,channel,rssi,dist_m"
#define DEV_LOG_HEADER "uptime_s,mac,name,rssi,dist_m"

#define NFC_FULL_NAME  "Jane Doe"
#define NFC_DOB        "1990-01-01"
#define NFC_BLOOD      "O+"
