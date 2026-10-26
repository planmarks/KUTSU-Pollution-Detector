# KUTSU Sense: Data Contract (v4)

[← KUTSU 1.0](../README.md) · [Firmware setup](README.md) · [Internals](FIRMWARE.md)

The firmware logs **one JSON object per line** (JSON Lines / NDJSON) to the MicroSD card as
`/kutsu_log.jsonl`. Without a card it logs to internal flash: `/kutsu_log.jsonl`, plus the
rotated `/kutsu_log.1.jsonl`. Each line is a complete sample. Every transport uses this same
record:

- **SD / flash → you:** read the file, split on newlines and `JSON.parse` each line.
- **KUTSU Hub (Wi-Fi, stationary mode):** the unit `POST`s its current record to the hub's
  `/api/live` every 5 s, and streams new log lines verbatim (NDJSON) to `/api/ingest?src=log`.
  The hub stores them in SQLite. See [`../KutsuHub/README.md`](../KutsuHub/README.md).
- **BLE:** the LIVE characteristic notifies the same record; HISTORY reads back the log file.

The single source of truth is `buildRecordJson()` in `KutsuSense/KutsuSense.ino`. Keep this
document in sync with it, and bump `SCHEMA_VERSION` in `config.h` on any change.

## v4 changes (from v3)

- **`lat` / `lon` / `loc_ts` are back.** The device has no GPS, but a connected phone /
  desktop app / USB host can now supply a fix (over BLE `TIMEGEO` or the serial `geo`
  command). The fields are **always present**, and `null` until a fix is supplied.
- **Time now has multiple sources.** `ts` is set by NTP (when on Wi-Fi) **or** by a
  connected device (BLE `TIMEGEO` / serial `time`). The `timesync` anchor's `src` is
  `"ntp"`, `"app"` (BLE), or `"usb"` (serial).
- **New `geo` anchor line** records a fix for session back-fill (see below).

## v3 changes (from the Stationary v2 contract)

- **Sensor set varies per unit.** A metric key appears **only if that sensor is fitted** on
  this unit (detected at boot). A fitted sensor with no valid reading this sample logs `null`.
- **New metrics:** `co2` (SCD41, true NDIR), `voc` and `nox` (SGP41 index). The CCS811 keys
  `eco2`/`tvoc` remain for DIY breakouts.
- **New field `hw`:** the XIAO module, e.g. `"XIAO ESP32-C6"`.
- **`ts` comes from NTP** once the unit joins Wi-Fi. *(v4 adds phone/BLE and USB time
  sources too.)*
- **Removed `lat`/`lon`/`loc_ts`** — *reinstated in v4, now that a connected device can
  supply a fix.*

## Record shape

KUTSU 1.0 board, on the network, with an MQ-2 fitted:

```json
{"v":4,"id":"Kutsu-A1B2C3","mac":"AA:BB:CC:A1:B2:C3","hw":"XIAO ESP32-C6","role":"slave",
 "room":"Workshop","up":1234,"ts":1791000000,"lat":51.5074,"lon":-0.1278,"loc_ts":1791000000,
 "sev":1,"alarm":0,"fault":0,"co2":1180,"voc":132,"nox":4,"temp":22.6,"hum":44.8,"gas":1503,
 "s_co2":1,"s_voc":0,"s_nox":0,"s_temp":0,"s_hum":0,"s_gas":0}
```

DIY breakouts, offline (handheld), no time/location source connected:

```json
{"v":4,"id":"Kutsu-0F1E2D","mac":"AA:BB:CC:0F:1E:2D","hw":"XIAO ESP32-C3","role":"solo",
 "room":"","up":95,"ts":null,"lat":null,"lon":null,"loc_ts":null,"sev":0,"alarm":0,"fault":0,
 "eco2":412,"tvoc":6,"temp":23.1,"hum":41.0,"s_eco2":0,"s_tvoc":0,"s_temp":0,"s_hum":0}
```

| Field | Type | Unit / meaning |
|---|---|---|
| `v` | int | schema version (`SCHEMA_VERSION`, now `4`) |
| `id` | string | device name `Kutsu-<suffix>`, derived from the MAC. Dashboards key on this |
| `mac` | string | full Wi-Fi STA MAC. Stable unique id |
| `hw` | string | MCU module: `XIAO ESP32-C3` / `-C5` / `-C6` / `-S3` |
| `role` | string | `"solo"` (stand-alone: handheld, offline, or no hub reached) · `"slave"` (feeding a KUTSU Hub). `"master"` only appears in records from KUTSU Stationary (v2) nodes |
| `room` | string | user label from `ROOM_LABEL` or the `room` serial command (may be empty) |
| `up` | int | seconds since boot |
| `ts` | int \| null | **real epoch seconds** (UTC). `null` until a time source (NTP, the KUTSU Hub, or a connected phone/USB) answers |
| `lat` | number \| null | latitude (WGS-84 degrees) of the last fix from a connected device. `null` until supplied |
| `lon` | number \| null | longitude (WGS-84 degrees). `null` until supplied |
| `loc_ts` | int \| null | epoch seconds when that fix was taken (`null` if the clock wasn't synced yet) |
| `sev` | int | overall severity: `0` ok · `1` warning · `2` alert (max of all `s_*`) |
| `alarm` | 0/1 | any metric at alert level in this sample |
| `fault` | 0/1 | a detected sensor failed to initialise, or no environmental sensor was found |
| `co2` | int \| null | SCD41 CO₂, **ppm** |
| `eco2` | int \| null | CCS811 estimated CO₂, **ppm** |
| `tvoc` | int \| null | CCS811 TVOC, **ppb** |
| `voc` | int \| null | SGP41 VOC Index, **1–500** (100 = recent baseline) |
| `nox` | int \| null | SGP41 NOx Index, **1–500** (1 = baseline) |
| `temp` | number \| null | temperature, **°C**, 1 decimal (SHT40 › HTU21D › SCD41) |
| `hum` | number \| null | relative humidity, **%RH**, 1 decimal (same source as `temp`) |
| `gas` | int \| null | MQ-2 **raw ADC 0–4095** (relative, not ppm). Appears once a module is seen |
| `s_<metric>` | int | per-metric severity `0/1/2`, present for each metric key above |

## Anchor lines (time & geo)

When a time source first answers, the firmware appends **one** non-sample line. `src` is
`"ntp"` (Wi-Fi), `"hub"` (the KUTSU Hub's clock, on a LAN without internet), `"app"` (a
phone/desktop app over BLE), or `"usb"` (serial `time`):

```json
{"ev":"timesync","id":"Kutsu-A1B2C3","up":842,"ts":1791000000,"src":"ntp"}
```

It anchors uptime to epoch for the session. A consumer can back-fill absolute time for every
earlier `ts:null` line of the same boot as `anchor.ts − (anchor.up − line.up)`.

When a geolocation fix is supplied, a **`geo` anchor** is appended likewise (so a fix is
recoverable even if logs are trimmed):

```json
{"ev":"geo","id":"Kutsu-A1B2C3","up":842,"lat":51.5074,"lon":-0.1278,"loc_ts":1791000000}
```

Lines with an `"ev"` key are events, not samples, so ignore unknown `ev` values. A new boot
restarts `up` at 0.

## Rules for consumers

- **Missing key** means the unit doesn't have that sensor. **`null`** means the sensor is fitted
  but has no reading this sample (warming up, failed read). Render both as "—", never as `0`.
- Treat **unknown fields as optional**. Later versions only add fields; read `v` and branch if
  you must.
- Thresholds live in the firmware (`config.h`). Use `sev` / `s_*` for status colour rather than
  recomputing it, so the device and UI never disagree.
- **v2 compatibility:** KUTSU Stationary nodes send v2 records (`eco2`/`tvoc`/`temp`/`hum`/`gas`
  always present, plus `lat`/`lon`). The KUTSU Hub accepts and renders both.

## Sampling / retention

| Target | Rate | Retention |
|---|---|---|
| MicroSD | every sample (1 Hz) | until the card is full (~30 MB/day) |
| Internal flash | 1 in `FLASH_LOG_EVERY_N` (10) + every alarm sample | rotated at `FLASH_LOG_MAX` (384 KB). The current and previous file are kept, a few hours at 0.1 Hz |
