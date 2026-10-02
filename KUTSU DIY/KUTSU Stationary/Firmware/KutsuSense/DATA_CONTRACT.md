# KUTSU Sense — Data Contract (v2)

[← Stationary firmware](../README.md) · [Setup](README.md) · [Internals](FIRMWARE.md)

The firmware logs **one JSON object per line** (JSON Lines / NDJSON) to the SD card
(`/kutsu_log.jsonl`, or internal flash if no card). Each line is a complete sample.
This is deliberately the **same record** every transport sends, so the SD file, the BLE
app, the WiFi mesh push, and the master dashboard all consume one shape:

- **SD → app/dashboard:** read the file; split on newlines; `JSON.parse` each line.
- **BLE:** the LIVE characteristic notifies one JSON record per sample; the HISTORY
  characteristic returns the tail of the same file for on-device storage.
- **WiFi mesh:** a slave `POST`s each record to the master's `/api/ingest`; browsers poll
  the master's `GET /api/nodes` every 2 s. Batching as `[ {...}, {...} ]` is valid (same
  per-object shape).

Producer of the string: `buildRecordJson()` in `KutsuSense.ino` — the single source
of truth. Keep this document in sync with it; bump `v` / `SCHEMA_VERSION` on changes.

## v2 changes (from v1)
- `dev` (fixed `DEVICE_ID`) **→ `id`** (runtime MAC-derived name, e.g. `"Kutsu-A1B2C3"`).
- Added `mac`, `role`, `room` (node identity for the multi-node dashboard).
- Added `ts` (real epoch seconds, `null` until a phone syncs the clock over BLE).
- Added `lat` / `lon` / `loc_ts` (last fix pushed by the phone; `null` until set).
- New non-sample line type: the **`timesync` anchor** (see below).

## Record shape

```json
{"v":2,"id":"Kutsu-A1B2C3","mac":"AA:BB:CC:A1:B2:C3","role":"master","room":"Kitchen",
 "up":1234,"ts":1719000000,"sev":1,"alarm":0,"fault":0,
 "eco2":812,"tvoc":140,"temp":22.6,"hum":44.8,"gas":1503,
 "s_eco2":0,"s_tvoc":0,"s_temp":0,"s_hum":0,"s_gas":1,
 "lat":51.5074,"lon":-0.1278,"loc_ts":1719000000}
```

| Field | Type | Unit / meaning |
|---|---|---|
| `v` | int | schema version (`SCHEMA_VERSION`) |
| `id` | string | runtime device name `Kutsu-<suffix>` (MAC-derived) — dashboards/app key on this |
| `mac` | string | full device MAC (also the BLE address) — stable unique id |
| `role` | string | `"solo"` (no WiFi/mesh yet) · `"master"` (hosts dashboard) · `"slave"` (feeds master) |
| `room` | string | user-set room label (empty until provisioned) |
| `up` | int | uptime seconds since boot |
| `ts` | int \| null | **real epoch seconds** (`bootEpoch + up`); `null` until a phone syncs the clock |
| `sev` | int | overall severity: `0` ok · `1` warning · `2` alert (max of all `s_*`) |
| `alarm` | 0/1 | any metric at alert level this sample (drives the buzzer/red LED) |
| `fault` | 0/1 | a required sensor failed to initialise (device error / yellow-blink) |
| `eco2` | int \| null | CCS811 eCO₂, **ppm** (`null` if the sensor isn't valid) |
| `tvoc` | int \| null | CCS811 TVOC, **ppb** |
| `temp` | number \| null | HTU21D temperature, **°C** |
| `hum` | number \| null | HTU21D relative humidity, **%RH** |
| `gas` | int \| null | MQ-2 **raw ADC 0–4095** (relative level, not ppm — see note); `null` if absent/unpowered |
| `s_eco2` `s_tvoc` `s_temp` `s_hum` `s_gas` | int | per-metric severity `0/1/2` (so the UI can colour each tile without re-deriving thresholds) |
| `lat` `lon` | number \| null | last geolocation fix from the phone (WGS-84 degrees); `null` until set |
| `loc_ts` | int \| null | epoch seconds of that fix; `null` until set |

## Time-sync anchor line (req 2.2)
When a phone syncs the clock over BLE the firmware appends **one** non-sample line:
```json
{"ev":"timesync","id":"Kutsu-A1B2C3","up":842,"ts":1719000000,"lat":51.5074,"lon":-0.1278}
```
It anchors `uptime → epoch` for the whole session, so a consumer can back-fill absolute
time for every earlier `ts:null` line as `epoch − (anchor.up − line.up)`. Lines carrying
an `"ev"` key are events, not samples — ignore unknown `ev` values.

### Rules for consumers
- **`null` means "no reading"** (sensor missing/failed/warming up) — render as "—", not `0`.
- Treat any **unknown field as optional** and ignore it; only add fields in later `v`s,
  don't repurpose existing ones. Read `v` and branch if you must.
- Thresholds live in firmware (`config.h`); trust `sev` / `s_*` for status colour rather
  than re-computing, so the device and UI never disagree.

### Notes / roadmap
- **Time:** `ts` is real epoch seconds once a phone syncs the clock over BLE (`bootEpoch + up`);
  before that it is `null` and `up` (uptime) is the only ordering key. There is still no RTC —
  the phone is the clock source (WiFi NTP is an optional later refinement).
- **Geolocation:** `lat`/`lon` come from the phone (the device has no GPS); they mark where a
  node was provisioned and stay fixed per room. `null` until the phone pushes a fix.
- **Gas units:** MQ-2 `gas` is a raw ADC level, not calibrated ppm. Calibrate `GAS_WARN` /
  `GAS_HARM` in `config.h` against a real gas/smoke test; the dashboard can map raw→label
  or show the trend. `s_gas` already reflects the firmware's warn/alert decision.
- **Batching:** for WiFi/BLE, sending an array of these objects `[ {...}, {...} ]` per
  flush is fine — same per-object shape.

## Example (three consecutive lines, clock already synced)
```
{"v":2,"id":"Kutsu-A1B2C3","mac":"AA:BB:CC:A1:B2:C3","role":"slave","room":"Kitchen","up":10,"ts":1719000010,"sev":0,"alarm":0,"fault":0,"eco2":410,"tvoc":3,"temp":22.4,"hum":45.1,"gas":1490,"s_eco2":0,"s_tvoc":0,"s_temp":0,"s_hum":0,"s_gas":0,"lat":51.5074,"lon":-0.1278,"loc_ts":1719000000}
{"v":2,"id":"Kutsu-A1B2C3","mac":"AA:BB:CC:A1:B2:C3","role":"slave","room":"Kitchen","up":11,"ts":1719000011,"sev":1,"alarm":0,"fault":0,"eco2":965,"tvoc":210,"temp":22.5,"hum":45.0,"gas":1512,"s_eco2":1,"s_tvoc":0,"s_temp":0,"s_hum":0,"s_gas":0,"lat":51.5074,"lon":-0.1278,"loc_ts":1719000000}
{"v":2,"id":"Kutsu-A1B2C3","mac":"AA:BB:CC:A1:B2:C3","role":"slave","room":"Kitchen","up":12,"ts":1719000012,"sev":2,"alarm":1,"fault":0,"eco2":2100,"tvoc":260,"temp":22.5,"hum":45.0,"gas":1520,"s_eco2":2,"s_tvoc":0,"s_temp":0,"s_hum":0,"s_gas":0,"lat":51.5074,"lon":-0.1278,"loc_ts":1719000000}
```
