# KUTSU — device setup & provisioning contract

How a Kutsu Sense device is set up locally — the single source of truth shared by
the **firmware** (`KutsuFirmwareXIAOESP32Series`) and the **Kutsu Setup app**
(`KutsuSetupApp/`). Keep both sides in sync with this document.

## Why

A flashed Kutsu with no network is a standalone handheld (display + alarm + log).
To feed the [KUTSU Hub](KutsuHub/README.md) (the LAN dashboard on a Raspberry Pi / PC) it needs
stationary mode and WiFi credentials.
That used to mean hand-writing bytes to a GATT characteristic in **nRF Connect** —
accurate but unfriendly. **Kutsu Setup** ([`KutsuSetupApp/`](KutsuSetupApp/)) is a
small local app that does it in a form, over **Bluetooth** or **USB**, with no
install and no cloud. This file defines exactly what it talks to.

## Two transports, one job

| | **Bluetooth LE** | **USB (serial)** |
|---|---|---|
| When | Wireless, device already powered in place | Cable handy; works even with no radio set up |
| Firmware | GATT server (`ble_provisioning.cpp`, `PROVISION_BLE`) | Console in `KutsuSense.ino` (`handleCommand`) |
| App API | Web Bluetooth | Web Serial |

Both carry the **same information** (WiFi SSID/password, room label, clock) so the
app offers whichever the user prefers.

---

## 1. Bluetooth LE — "Kutsu Environmental Service"

**Advertising:** the device advertises its unique name **`Kutsu-XXXXXX`** (the last
3 MAC bytes) and the service UUID below; it also exposes the standard **Device
Information Service `0x180A`** (serial number `2A25` = full MAC, manufacturer
`2A29` = "Papathought", model `2A24` = "Kutsu Sense"). It keeps advertising after a
central disconnects, so it is always reachable for the next session.

**Service UUID:** `6b755473-7500-4b75-7473-750000000001`

| Characteristic | UUID (suffix) | Properties | Payload |
|---|---|---|---|
| **WIFI** | `…0004` | Write | `{"ssid":"..","password":"..","room":".."}` — `room` optional; sets credentials, saves to NVS, reconnects. An empty `ssid` disables WiFi. |
| **STATUS** | `…0005` | Read / Notify | `{"name","mac","role","ip","room","synced","ts","v"}` — see below. Read is refreshed live; notifies on change. |
| **LIVE** | `…0002` | Read / Notify | One JSON sample record per notification (the `DATA_CONTRACT.md` shape). |
| **TIMEGEO** | `…0003` | Write | `{"epoch":<unix seconds>[,"lat":..,"lon":..]}` — sets the clock and/or the geolocation. `epoch` and the `lat`/`lon` pair are applied independently; both are then stamped onto every record (`ts` / `lat` / `lon` / `loc_ts`). |
| **SETTINGS** | `…0007` | Read / Write / Notify | `{"name","buzzer","led","display","scan","mode","buzzer_sig","led_warn","led_alert"}` — device settings (§3). Read/notify returns live state; write applies any subset and notifies the new state. |
| **THRESHOLDS** | `…0008` | Read / Write | Per-metric warn/alert levels (§3). Read returns all; write applies any subset; `{"reset":true}` restores the firmware defaults. |
| **HISTORY** | `…0006` | *(reserved)* | Defined for cross-build compatibility; **not served** by the XIAO-series build yet (the Stationary build returns a log tail here). |

**STATUS fields**

| Field | Meaning |
|---|---|
| `name` | `Kutsu-XXXXXX` (MAC-derived, stable) |
| `mac` | full MAC, e.g. `AA:BB:CC:A1:B2:C3` |
| `role` | `solo` (stand-alone / no hub reached) · `slave` (feeding a KUTSU Hub) |
| `ip` | own IP while online, else `""` |
| `hub` | `ip:port` of the KUTSU Hub being fed (dashboard at `http://<hub>/`), else `""` |
| `room` | user-set room label, `""` until set |
| `synced` | `true` once the clock has been set (over BLE or NTP) |
| `ts` | current epoch seconds, or `0` if unsynced |
| `v` | data-contract schema version |

**Minimal provisioning flow:** connect → write SETTINGS `{"mode":"stationary"}` → (optional)
subscribe STATUS → write WIFI `{ssid,password,room}` → poll/await STATUS until `hub` is
non-empty → open `http://<hub>/`. The app also writes TIMEGEO with the computer's clock on connect.

---

## 2. USB — serial console (115200 baud, 8N1)

Line-based text. **`USB CDC On Boot` must be enabled** (Serial is over USB-C).
Send one command per line (`\n`-terminated):

| Command | Effect |
|---|---|
| `ssid <name>` | stage the SSID |
| `pass <password>` | stage the password |
| `room <label>` | set + persist the room label |
| `connect` | save staged SSID/password to NVS and join |
| `forget` | clear credentials (back to handheld) |
| `hub <host[:port]>` / `hub auto` | pin the KUTSU Hub address (networks that block broadcasts) / back to auto-discovery |
| `status` | print one `[status] {json}` line (same shape as the BLE STATUS payload) |
| `settings` | print one `[settings] {json}` line (same shape as the BLE SETTINGS payload) |
| `set <field> <on\|off>` | toggle `buzzer` / `led` / `display` / `scan` (§3); echoes `[settings] {…}` |
| `mode <handheld\|stationary>` | switch mode (§3); echoes `[settings] {…}` |
| `time <epoch>` | set the clock from the USB host (Unix seconds) |
| `geo <lat> <lon>` | set the geolocation from the USB host (WGS-84 degrees) |
| `name <text>` | set the device name; empty reverts to the creature name (4.4/4.5) |
| `buzzer <0-4>` | buzzer signal: 0 triple · 1 single · 2 long · 3 rapid · 4 SOS — plays a preview (4.2) |
| `led <warn\|alert> <0-4>` | LED behaviour: 0 off · 1 slow · 2 fast · 3 solid · 4 SOS (4.3) |
| `thresholds` | print `[thresholds] {json}` |
| `threshold <metric> <warn> <harm>` | set warn/alert for co2/eco2/tvoc/voc/nox/gas (temp/hum via the app/BLE) (4.1) |

The device also emits bracketed log lines (`[con] …`, `[ble] …`, `[main] …`,
`[settings] …`) and — to any log consumer — JSON sample records. The app parses
`[status] {…}` / `[settings] {…}` for device state and JSON records for live readings.

---

## 3. Device settings & behavior model

### Settings (read/write when connected)

A connected client (desktop app / mobile app / local webapp) may change these; they
persist in NVS and default to **everything on, mode = handheld** on first launch.
JSON shape: `{"buzzer":1,"led":1,"display":1,"scan":1,"mode":"handheld"}`.

| Setting | Effect when off |
|---|---|
| `buzzer` | no audible alarm (screen/LED alerts still fire) |
| `led` | warning LED stays dark |
| `display` | OLED powered off — the device keeps sensing, logging and serving |
| `scan` | stops passive WiFi/BLE scanning (**BLE setup advertising stays on**) |
| `mode` | `handheld` ⇄ `stationary` — see below (not an on/off) |

**Also configurable (4.x):**

| Field | Meaning |
|---|---|
| `name` | device display name (4.5). A random **creature name** is generated at first boot (4.4); a custom name overrides it, and an empty value reverts to the creature name. The MAC stays the stable unique id. |
| `buzzer_sig` | alarm signal pattern `0–4` (triple / single / long / rapid / SOS). Changing it plays a **preview** on the device (4.2). |
| `led_warn` / `led_alert` | LED behaviour `0–4` (off / slow blink / fast blink / solid / SOS) for warning vs. alert severity (4.3). |
| **thresholds** | per-metric warn/alert levels (4.1), on the separate `THRESHOLDS` characteristic / `threshold` serial command. Simple metrics carry `{w,h}`; `temp` carries `{wh,hh,hl}`; `hum` carries `{hl,wl,wh,hh}`. The firmware's `config.h` values are the defaults; `{"reset":true}` restores them. |

### Behavior model

- **First launch / default:** handheld mode. The device scans for WiFi + BLE **and
  advertises itself** for setup. Connecting a phone over BLE does not change this —
  scanning continues.
- **Settings unlock:** device settings are only reachable through an active management
  connection: **USB or Bluetooth** (desktop app, mobile app, serial). The KUTSU Hub only
  receives data and cannot change device settings. With nothing connected there is no way in;
  that *is* the lock.
- **Handheld:** never joins WiFi, even with saved credentials. Data stays on the unit (SD/flash)
  and is reachable over BLE (LIVE, HISTORY read-back) or USB.
- **Stationary + WiFi:** the unit finds the **KUTSU Hub** on the LAN (UDP broadcast on port
  47800, or a pinned `hub` address). It sends its live reading every 5 s and streams its log, so
  the hub has the full history, including whatever was logged while the hub was unreachable.
  The dashboard is served by the hub (`http://<hub>:8080/`), not by the unit. Without a hub the
  unit keeps working stand-alone and keeps looking.
- **Switching mode at runtime** (via any connection) is applied without a reboot: the
  net task tears down the current role and re-evaluates against the new mode.

---

## 4. Security & scope

- **Local, unauthenticated, by design.** Provisioning **and settings** are open (no
  PIN/bonding) so setup is friction-free; anyone within Bluetooth range or with the USB
  cable can set the network and toggle settings — this is the "connected = unlocked"
  model. It suits the current single-user/local-admin case. **If devices are deployed
  where that matters, add BLE bonding / a pairing PIN before shipping** — a known item.
- **Credentials at rest:** stored in the ESP32 NVS (`kutsu` namespace). They are not
  exposed by any read characteristic.
- **No cloud:** everything here is device-local. The dashboard is served by the KUTSU Hub
  on your LAN.

## 5. Firmware notes (XIAO-series build)

- The provisioning GATT server is layered **on top of** the passive BLE scanner —
  `ble_provisioning.cpp` does **not** call `BLEDevice::init()` (the scanner already
  did). It adds the server + advertising only.
- **`BLEDevice::init("")` must be called with an EMPTY name.** Passing a device name
  there asserts in NimBLE's `ble_svc_gap_init` (`ble_svc_gap.c:611`, `rc==0`) on the
  C5/C6 when a GATT server is present → boot loop. The advertised name `Kutsu-XXXXXX`
  is set on the advertising object in `bleProvisioningStart()` instead.
- Set `PROVISION_BLE 0` in `config.h` to drop the BLE server if a module still
  misbehaves (watch the single-radio C3).
- **Verify on hardware:** server+scanner+WiFi coexistence on one 2.4 GHz radio is
  heavier than the Stationary case (which doesn't scan). Confirm advertising stays
  up and scans/joins still work; tune BLE scan duty (`BLE_SCAN_*` in `config.h`) if
  the radio is starved.
- **Settings & mode** live in `settings.cpp` (NVS-backed). The 1 Hz loop reads the
  buzzer/LED/display/scan toggles each tick. `mesh.cpp` reads `mode`: `STATIONARY` joins
  WiFi and feeds the KUTSU Hub, `HANDHELD` stays off WiFi. A runtime mode change is picked up
  within about half a second.
- **Hub-side settings** (changing a unit's thresholds or name from the KUTSU Hub dashboard)
  are not implemented. The hub is receive-only. Doing it would need a small command channel,
  for example settings piggy-backed on the hub's reply to `/api/live`. Settings remain
  BLE/USB-only.

## 6. Roadmap for the app

Kutsu Setup is intentionally a single self-contained file and the seed for more:
firmware version/OTA, SCD41 fresh-air (FRC) calibration, threshold tuning, naming
nodes, and a packaged Tauri/Electron desktop build. New features extend both
transports behind the app's common interface — no protocol fork.
