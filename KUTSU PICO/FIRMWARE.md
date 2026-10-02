# KUTSU PICO — Firmware

[← KUTSU PICO](README.md) · [Hardware](HARDWARE.md)

CircuitPython firmware for the Pimoroni Pico LiPo prototype. CircuitPython was chosen because
it has the best ready-made driver coverage for this mostly-Adafruit BOM, including Sensirion's
VOC-index algorithm (bundled in `adafruit_sgp40`).

---

## Installation

1. **Flash CircuitPython 10.x** for the *Pimoroni Pico LiPo (16MB)*: hold BOOTSEL, plug in USB,
   and drop the `.uf2` onto `RPI-RP2`.
2. **Copy the libraries.** The `lib/` folder already holds the required bundle files (see
   [lib/README.md](lib/README.md)). Copy it to `CIRCUITPY/lib`.
3. **Copy the firmware:** `code.py`, `boot.py`, `config.py`, `kutsu/` and `assets/`.
4. **Reset.** The welcome logo appears, followed by live readings.
5. Open a serial console (115200) to see the I²C scan and the init messages.

---

## Main loop (`code.py`)

```text
boot:  release displays → I2C scan → Sensors()  → Display() → welcome
       → History, Logger (SD → flash), Buzzer, Button → NFC provision → 2 s pause

loop:  every pass   ─ buzzer.update() · button events (toggle live ⇄ charts)
       every 1 s    ─ sensors.read() → history.add() → alarms.evaluate()
                      → alarm show/gap state machine or normal screen → logger.log()
```

**Alarm state machine.** While a reading is harmful, the full-screen alarm shows for
`ALARM_SHOW_S` (5 s), then the normal screen shows for `ALARM_GAP_S` (1 s), and the cycle
repeats. The SOS plays **once** when the device enters a harmful state, and the latch resets
when the readings return to normal.

---

## Modules

| Module | Responsibility | Highlights |
|---|---|---|
| `sensors.py` | Reads all sensors into a dict | HTU21D is read first because it feeds temp/RH compensation to the SGP40. The SGP40 self-test (which this breakout NAKs) is patched out, and reads retry 3× and hold the last good value. CCS811 is read when `data_ready`. MQ-2 is a raw 16-bit ADC value. **Any missing chip returns `None` and is skipped instead of crashing the device** |
| `display.py` | OLED screens via `displayio` | Pre-built groups are swapped onto the root, and only label text and chart bitmaps change each frame |
| `history.py` | One-hour charts | 60 one-minute running means for VOC, temperature and humidity |
| `alarms.py` | Threshold evaluation | Pure logic with no hardware, so it can be unit-tested on desktop Python. Returns per-metric severity plus a list of harmful readings |
| `logging_store.py` | CSV logging | SD if it mounts, otherwise flash. When flash passes the cap, the oldest ~25% of rows are dropped. If the filesystem is read-only, rows are queued in RAM |
| `nfc.py` | NT3H2111 writer | Builds the NDEF text record by hand (there's no CircuitPython driver) and writes it once at boot |
| `buzzer.py` | Alarm sound | Non-blocking Morse **SOS** (`· · · — — — · · ·`, 150 ms unit) on an active buzzer |

### Sensor readings

| Key | Sensor | Unit | Shown | Charted | Alarmed | Logged |
|---|---|---|---|---|---|---|
| `voc_index` | SGP40 | index 0–500 | ✅ | ✅ | ✅ (after 60 s warm-up) | ✅ |
| `eco2_ppm` | CCS811 | ppm | ✅ | — | — | — |
| `tvoc_ppb` | CCS811 | ppb | ✅ | — | — | — |
| `mq2_raw` | MQ-2 | raw ADC 0–65535 | ✅ | — | — | — |
| `temp_c` | HTU21D | °C | ✅ | ✅ | ✅ | ✅ |
| `humidity_pct` | HTU21D | %RH | ✅ | ✅ | ✅ | ✅ |

---

## Configuration (`config.py`)

Every tunable lives in one file, so you can re-pin or re-tune the device without touching the
modules.

| Group | Keys |
|---|---|
| Buses | `I2C_SDA`, `I2C_SCL`, `I2C_FREQUENCY`, `SD_SCK/MOSI/MISO/CS`, `SD_BAUDRATE` |
| Controls | `BUTTON_PIN`, `BUZZER_PIN`, `MQ2_AO_PIN` |
| Timing | `SAMPLE_INTERVAL_S` (1), `CHART_BIN_SECONDS` (60), `CHART_BINS` (60), `ALARM_SHOW_S`, `ALARM_GAP_S` |
| Logging | `LOG_CSV_HEADER`, `SD_LOG_PATH`, `FLASH_LOG_PATH`, `FLASH_LOG_MAX_BYTES` (256 KB) |
| Alarms | `THRESHOLDS` (per metric: `warn_low/high`, `harmful_low/high`, message), `VOC_WARMUP_S` |
| NFC | `NFC_RECORD_TYPE`, `NFC_HEALTH` (`full_name`, `date_of_birth`, `blood_type`) |

### Thresholds

| Metric | Warning `!` | Harmful | Alarm message |
|---|---|---|---|
| VOC index | ≥ 150 | ≥ 400 | `POOR AIR QUALITY / Ventilate now` |
| Temperature | ≥ 50 °C | ≥ 70 °C | `UNSAFE TEMPERATURE` |
| Humidity | ≥ 55 % | ≥ 70 % | `UNSAFE HUMIDITY` |

Low-side (cold and dry) warnings are disabled because the prototype focuses on high-heat and
high-humidity hazards. Set `warn_low` or `harmful_low` to turn them back on.

---

## Logging

| | |
|---|---|
| **SD** | `/sd/kutsu_log.csv` (effectively unbounded) |
| **Flash fallback** | `/kutsu_log.csv`, capped at 256 KB; the oldest rows are trimmed first |
| **Header** | `uptime_s,voc_index,temp_c,humidity_pct,alarm` |
| **Timestamps** | Seconds since power-on (no RTC) |

### `boot.py` and flash writes

CircuitPython can only write to its own flash when `boot.py` remounts `/` read-write, and while
it does, the **USB drive is read-only to your computer**.

| At power-on | Result |
|---|---|
| Button **not** held | Flash is writable by the firmware, so flash logging works |
| Button **held** (GP15) | The USB drive stays editable and flash logging is off |

If you only ever log to SD, you can delete `boot.py`.

---

## NFC health record

`nfc.py` writes `config.NFC_HEALTH` as a plain-text NDEF record once at boot:

```python
NFC_HEALTH = {
    "full_name": "Jane Doe",
    "date_of_birth": "1990-01-01",
    "blood_type": "O+",
}
```

> [!CAUTION]
> The NT3H2x11 block placement (capability container in block 0, user memory from block 1)
> **still has to be checked against the datasheet**. The tag can also be read by anyone with a
> phone, so only store information you're happy to share.

---

## Bring-up and test scripts

Copy one of these over `code.py`, reboot, read the serial output, then restore the real
`code.py`.

| Script | Purpose |
|---|---|
| `i2c_scan.py` | Scans every 3 s and marks each expected device `OK` or `MISS` |
| `sd_test.py` | Mounts the card with built-in `sdcardio`, lists it and writes a test file |
| `sd_test2.py` | Same test with `adafruit_sdcard`, whose different timing sometimes mounts cards `sdcardio` can't |
| `sgp_test.py` | Raw SGP40 constructor, so you get the full traceback from the driver |

### Welcome logo

`assets/make_welcome_logo.py` (needs Pillow) regenerates the 128 × 64 1-bit
`welcome_logo.bmp`, an 8× preview PNG, and a framebuf byte-array fallback. The current art is a
placeholder.

```bash
python assets/make_welcome_logo.py
```

---

## Known issues and limitations

- **No RTC.** Timestamps are uptime-relative.
- **CCS811 and MQ-2 are display-only.** They have no thresholds, history or log columns.
- **SGP40 warm-up.** The index reads about 100 and isn't meaningful for the first ~60 s.
- **Marginal I²C bus** on the protoboard. The firmware already runs at 50 kHz with read
  retries; try 25 kHz if errors continue.
- **CircuitPython has no ESP32-C5 support**, which is why the project moved to Arduino/C++
  for the next generation.
