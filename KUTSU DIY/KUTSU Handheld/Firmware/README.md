# KUTSU Handheld — Firmware

[← KUTSU Handheld](../README.md) · [Hardware](../HARDWARE.md)

Arduino/C++ firmware for the Handheld board. There are **two folders, one per XIAO module**.
The logic is the same in both; they differ only in radio and board details.

---

## Which one do I need?

| | [**KutsuFirmwareXIAOESP32C5**](KutsuFirmwareXIAOESP32C5/README.md) | [**KutsuFirmwareXIAOESP32C3**](KutsuFirmwareXIAOESP32C3/README.md) |
|---|---|---|
| **Module** | Seeed XIAO ESP32-C5 | Seeed XIAO ESP32-C3 |
| **Wi-Fi bands** | 2.4 GHz **+ 5 GHz** | 2.4 GHz only |
| **Cores** | 1 | 1 |
| **Arduino board** | `XIAO_ESP32C5` (core with C5 support) | `XIAO_ESP32C3` (any recent core) |
| **SD on D8/D9** | ✅ No conflicts | ⚠️ Strapping pins: may block boot with a card inserted |
| **Status** | First port from the RP2040 build | Retargeted from the C5 build |

Each folder contains:

| File | Purpose |
|---|---|
| `README.md` | Board-package setup, libraries, wiring, features, bring-up checklist |
| `FIRMWARE.md` | Firmware internals: architecture, UI, logging, known issues |
| `KutsuSense/KutsuSense.ino` | The sketch |
| `KutsuSense/config.h` | **Every tunable:** pins, thresholds, timings, NFC record |

---

## Quick start

1. **Arduino IDE → Boards Manager →** install **esp32 by Espressif**.
2. Select **XIAO_ESP32C5** or **XIAO_ESP32C3**.
3. Set **Tools → Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)**. Wi-Fi + BLE +
   graphics overflow the default 1.2 MB app partition.
4. Install these libraries: **Adafruit SSD1306**, **Adafruit GFX**, **Adafruit HTU21DF**,
   **Adafruit CCS811**, **Adafruit BusIO** and **NimBLE-Arduino** (2.x).
5. Open `KutsuSense/KutsuSense.ino` and upload.
6. Open the Serial Monitor at **115200**. The I²C scan should report `0x3C 0x40 0x55 0x5A`.

---

## What the firmware does

```text
loop()  ── every 1 s ──▶ readSensors → historyAdd → evaluate → render | alarm → logRow
        ── every pass ─▶ handleButtons · updateBuzzer · serviceScans (Wi-Fi / BLE)
```

| Feature | Behaviour |
|---|---|
| **Screens** | Live → Stats (4 metrics × 60 one-minute bars) → Networks → Devices (D6 cycles) |
| **Alarm** | A pulsing full-screen WARNING and 3 beeps once; D6 dismisses it; re-arms after recovery |
| **Logging** | `/kutsu_log.csv` on SD, LittleFS flash fallback |
| **Scan logs** | `/kutsu_net.csv`, `/kutsu_dev.csv` about once a minute (SD only) |
| **NFC** | Plain-text health record written to the NT3H2111 at boot |
| **Wi-Fi / BLE** | Passive background scanners with RSSI distance estimates. No outbound link |

**CSV header:** `uptime_s,eco2_ppm,tvoc_ppb,temp_c,humidity_pct,alarm`

---

## Personalising the NFC record

Edit these values in `config.h` before flashing:

```cpp
#define NFC_FULL_NAME  "Jane Doe"
#define NFC_DOB        "1990-01-01"
#define NFC_BLOOD      "O+"
```

> [!CAUTION]
> The NFC tag can be read by **anyone with a phone**. Store only information you're happy
> to share.
