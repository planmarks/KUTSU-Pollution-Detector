"""
KUTSU Sense — central configuration.

Everything tunable lives here: pin assignments, timing, alarm thresholds, and the
fixed NFC health record. Keep hardware-specific magic numbers OUT of the modules
and IN this file so the prototype can be re-pinned without code spelunking.

Pins reference the Pimoroni Pico Lipo (RP2040). I2C defaults to the STEMMA/Qwiic
pins (GP4/GP5); verify against https://pico.pinout.xyz/pimoroni-pico-lipo when wiring.
"""

# --- I2C bus (SGP40, HTU21D, SSD1306 OLED, NTAG all share this bus) ---
I2C_SDA = "GP4"
I2C_SCL = "GP5"
I2C_FREQUENCY = 50_000           # 50 kHz — lowered for a marginal protoboard bus; try 25_000 if still flaky

# I2C addresses (for bus scanning / sanity checks)
ADDR_SGP40 = 0x59
ADDR_HTU21D = 0x40
ADDR_SSD1306 = 0x3C
ADDR_NTAG = 0x55                 # NT3H2111 default
ADDR_CCS811 = 0x5A               # CCS811 eCO2/TVOC (ADD pin tied low)

# --- SPI bus (MicroSD breakout) ---
SD_SCK = "GP18"
SD_MOSI = "GP19"
SD_MISO = "GP16"
SD_CS = "GP17"

# --- Controls ---
BUTTON_PIN = "GP15"              # screen-toggle push button (model TBD)
BUZZER_PIN = "GP14"             # active buzzer (SOS on warning)
MQ2_AO_PIN = "GP26"             # MQ-2 analog out via 10k/20k divider -> ADC0

# --- Display ---
OLED_WIDTH = 128
OLED_HEIGHT = 64

# --- Timing ---
SAMPLE_INTERVAL_S = 1            # live readings + display refresh
CHART_BIN_SECONDS = 60           # one chart column per minute
CHART_BINS = 60                  # 60 columns -> 1 hour of history

# --- Alarm display behaviour ---
ALARM_SHOW_S = 5                 # max seconds a full-screen warning stays up
ALARM_GAP_S = 1                  # gap (normal readings shown) before it re-appears

# --- Logging ---
LOG_CSV_HEADER = "uptime_s,voc_index,temp_c,humidity_pct,alarm"
SD_BAUDRATE = 1_000_000          # conservative SPI speed for breadboard wiring
SD_LOG_PATH = "/sd/kutsu_log.csv"
FLASH_LOG_PATH = "/kutsu_log.csv"
FLASH_LOG_MAX_BYTES = 256 * 1024  # ring-buffer cap when no SD card present

# --- Alarm thresholds ---------------------------------------------------------
# Each metric: (warn_low, normal_low, normal_high, warn_high). A reading is
# HARMFUL (full-screen alarm) when it falls outside [warn_low, warn_high].
# Set a bound to None to disable that side. Rationale + sources in README.md.
THRESHOLDS = {
    "voc_index": {
        "label": "Air Quality",
        "unit": "idx",           # SGP40 VOC Index (0-500), not ppb
        # VOC index is single-sided: only high is bad.
        "harmful_high": 400,     # Sensirion "ventilate hard" band -> full-screen alarm
        "warn_high": 150,        # "!" from 150
        "harmful_low": None,
        "warn_low": None,
        "harmful_msg": "POOR AIR QUALITY\nVentilate now",
    },
    "temp_c": {
        "label": "Temperature",
        "unit": "C",
        "harmful_low": None,     # cold warnings disabled — high-heat hazard focus
        "warn_low": None,
        "warn_high": 50,         # "!" from 50 C
        "harmful_high": 70,      # full-screen alarm from 70 C
        "harmful_msg": "UNSAFE TEMPERATURE",
    },
    "humidity_pct": {
        "label": "Humidity",
        "unit": "%",
        "harmful_low": None,     # dry warnings disabled — high-humidity hazard focus
        "warn_low": None,
        "warn_high": 55,         # "!" from 55%
        "harmful_high": 70,      # full-screen alarm from 70%
        "harmful_msg": "UNSAFE HUMIDITY",
    },
}

# --- SGP40 warm-up ------------------------------------------------------------
# The SGP40 VOC index is relative and self-calibrating; it reports ~100 and is
# not meaningful until it has learned a baseline. Suppress VOC alarms until then.
VOC_WARMUP_S = 60

# --- Fixed NFC health record (written once to the NTAG) -----------------------
# Plain-text NDEF for now; swap NFC_RECORD_TYPE to "uri" later if desired.
NFC_RECORD_TYPE = "text"
NFC_HEALTH = {
    "full_name": "Jane Doe",
    "date_of_birth": "1990-01-01",
    "blood_type": "O+",
}
