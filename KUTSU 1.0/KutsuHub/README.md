# KUTSU Hub

[← KUTSU 1.0](../README.md) · [Firmware](../KutsuFirmwareXIAOESP32Series/README.md) · [Data contract](../KutsuFirmwareXIAOESP32Series/DATA_CONTRACT.md)

**The collector and dashboard for KUTSU units in stationary mode.** Run it on any always-on
machine on your local network, such as a Raspberry Pi or an old Windows PC. KUTSU units on the
same network find it by themselves and send it their readings. It keeps the history in a local
database and serves a dashboard to any browser on the LAN.

```text
 KUTSU (stationary) ──┐   UDP 47800: "where is the hub?" → "here, port 8080"
 KUTSU (stationary) ──┼──▶ HTTP 8080: live reading every 5 s + new log lines
 KUTSU (stationary) ──┘                          │
                                                 ▼
                         Raspberry Pi / PC: kutsu_hub.py ── kutsu_hub.db (SQLite)
                                                 │
                    any browser on the LAN ◀─────┘  http://<hub>:8080/
```

- **Nothing to install** besides Python 3.7+. It uses the standard library only, so there's no
  `pip` step, no internet connection, no cloud and no account.
- **Nothing to configure** on the units for the common case: they discover the hub over UDP
  broadcast.
- **No data lost to outages.** Each unit streams its own SD/flash log. If the hub or Wi-Fi goes
  down, the unit keeps logging and sends the backlog when the link returns.

## Dashboard

- **Overview:** one card per unit, grouped by room. Each card shows live values coloured by the
  unit's own warning/alert levels, one-hour sparklines, and online/offline state.
- **Alerts & events:** warnings, alerts, readings back to normal, units joining, going offline
  or coming back.
- **Per-unit view:** charts for 1 h, 6 h, 24 h, 7 d or 30 d, with the average line and a
  min–max band, plus min/avg/max per range and a hover readout.
- **CSV export** for one unit and range, or for all units over the last 24 h.
- **Rename / re-room** a unit on the hub, or **Forget** it and delete its history.

## Quick start

### Raspberry Pi (Raspberry Pi OS, any model with network)

```bash
cd ~ && cp -r /path/to/KutsuHub . && cd KutsuHub
```
```bash
python3 kutsu_hub.py
```

The log prints the dashboard URL, for example `http://192.168.1.20:8080/`. To start it
automatically on every boot, run it as a service:

```bash
sh install_pi.sh
```

### Windows (7 / 8 / 10 / 11)

1. Install Python from [python.org](https://www.python.org/downloads/). On Windows 7 use
   **Python 3.8.x**, the last version that supports it. Tick **"Add python.exe to PATH"**.
2. Copy the `KutsuHub` folder anywhere and double-click **`start_hub.bat`**.
3. When Windows Firewall asks, allow Python on **the type of network you're on**. Tick both
   Private and Public if unsure: many PCs mark home Wi-Fi as *Public*, and allowing only Private
   then silently blocks every unit. The hub needs inbound TCP 8080 (dashboard and data) and UDP
   47800 (discovery). To check the profile, run `Get-NetConnectionProfile` in PowerShell.
4. Open the URL shown in the window.

To start it at logon, put a shortcut to `start_hub.bat` in `shell:startup`. Alternatively, use
Task Scheduler with "Run whether user is logged on or not".

> [!TIP]
> Give the hub machine a **fixed IP address** (a DHCP reservation in your router). Discovery
> doesn't need it, but bookmarks and pinned unit settings will keep working.

## Connecting KUTSU units

Each unit needs Wi-Fi credentials and **stationary mode**. Use the
[Kutsu Setup app](../KutsuSetupApp/README.md) over Bluetooth or USB, or the USB serial console
at 115200 baud:

```text
mode stationary
ssid MyNetwork
pass secret
connect
room Workshop          (optional, groups units on the dashboard)
```

Within a few seconds of joining, the unit finds the hub. Its **Hub** screen (the last screen on
the button cycle) shows the hub address, and the unit appears on the dashboard.

**Networks that block broadcasts** (guest Wi-Fi, some mesh systems or VLANs) need the hub
address pinned on the unit:

```text
hub 192.168.1.20:8080      (pin the hub)
hub auto                   (back to auto-discovery)
status                     (shows mode, hub, backlog)
```

A handheld-mode unit never joins Wi-Fi. Switching a unit to stationary mode takes effect
without a reboot.

## Troubleshooting

Watch the hub window (or `journalctl -u kutsu-hub -f` on the Pi). Each unit that reaches the hub
logs `device <name> (<mac>) online from <ip>`. Each discovery request logs
`discovery: answered <name>`.

| Symptom | Cause / fix |
|---|---|
| Dashboard stays empty; no `answered` lines | The unit isn't asking. Check that it runs firmware **0.10 or newer** (0.9 and older have no hub client), that it's in **stationary** mode (`mode stationary`) and on the same Wi-Fi (`status` on its serial console). |
| `answered …` lines but the unit never comes online | The unit can't reach TCP 8080. Check the Windows firewall (see above) or another firewall on the hub machine. The unit's serial log shows `[hub] POST http://…/api/live failed: …`. |
| Unit says "Finding hub…" forever | Broadcasts are blocked (guest Wi-Fi, client isolation, VLANs, some mesh routers). Pin the hub on the unit: `hub <hub-ip>:8080`. |
| `another KUTSU Hub is running at …` | Two hubs on one network. Units take whichever answers first. Stop one, or pin units with `hub <ip:port>`. |
| `cannot listen on …:8080` / `cannot bind UDP 47800` | The hub is already running on this machine (another window or the service). Close it, or use `--port` / `--udp-port`. Units must then use the same UDP port (`HUB_UDP_PORT`). |
| A unit flips online/offline | Weak Wi-Fi. Check the RSSI on the unit's Hub screen. Below about -80 dBm, move the unit or the access point. |
| Charts lag the cards by a few seconds | Expected. Cards update every 5 s from the live push. Charts show stored samples, kept every `--store-every` s (default 10). |

## Options

```text
python3 kutsu_hub.py [--port 8080] [--db kutsu_hub.db] [--udp-port 47800]
                     [--store-every 10] [--retention-days 365] [--offline-after 30]
                     [--beacon-every 10] [--no-discovery] [--name "KUTSU Hub"] [-v]
```

| Option | Default | Meaning |
|---|---|---|
| `--port` | 8080 | HTTP port for the dashboard and the device API. Units learn it from discovery |
| `--db` | `kutsu_hub.db` next to the script | SQLite file. Copy it to back up all history |
| `--store-every` | 10 | Keep at most one sample per unit every N s, plus **every** severity change. `0` keeps all (about 7 MB per unit per day at 1 Hz) |
| `--retention-days` | 365 | Delete older samples and events. `0` keeps them forever |
| `--offline-after` | 30 | Seconds of silence before a unit is shown offline |
| `--udp-port` | 47800 | Discovery port. Must match `HUB_UDP_PORT` in the firmware |
| `--beacon-every` | 10 | Seconds between "I'm here" broadcasts. `0` means only answer probes |
| `--no-discovery` | off | Disable UDP entirely; units then need `hub <ip:port>` |

**Sizing:** at the default 10 s thinning, a unit produces about 8 600 rows (~1 MB) per day,
roughly 350 MB per unit per year. A Raspberry Pi 3 or newer, or any old PC, handles dozens of
units.

## Try it without hardware

```bash
python3 kutsu_hub.py
```
```bash
python3 simulate_device.py -n 3 --backfill-hours 24
```

The simulator speaks the firmware's protocol: discovery, live push and NDJSON log batches. It
fakes three units: two with the Sensirion stack and one with DIY CCS811/HTU21D breakouts.

## Device ↔ hub protocol

The source of truth is `kutsu_hub.py` (hub) and `KutsuSense/mesh.cpp` (firmware). Records use the
[data contract](../KutsuFirmwareXIAOESP32Series/DATA_CONTRACT.md).

| Step | Direction | Message |
|---|---|---|
| Discover | unit → broadcast UDP 47800 | `{"kutsu_discover":1,"id":"Otter-C3","mac":"AA:BB:…"}` |
| Answer / beacon | hub → unit (and broadcast every 10 s) | `{"kutsu_hub":1,"v":1,"port":8080,"name":"KUTSU Hub","time":<epoch>}` |
| Live reading | `POST /api/live` (every 5 s) | one record. Updates the card, alerts and online state; **not stored** |
| History | `POST /api/ingest?src=log` | NDJSON lines appended to the unit's log since it first reached the hub this boot, including any backlog from an outage. **Stored** |
| No-log fallback | `POST /api/ingest` | one record, stored, from a unit with neither SD nor flash logging |

- **Clock:** units take time from NTP. On a LAN without internet they take the hub's `time`
  instead, so the hub machine's clock should be right.
- **Dedup:** samples are unique on (MAC, timestamp, uptime). Re-sent log lines are ignored.
- **Device identity:** the hub keys units on their **MAC**. A unit renamed on the device keeps
  its history.

**Read API** (used by the dashboard, handy for scripts):

| Endpoint | Returns |
|---|---|
| `GET /api/devices` | all units with their latest record, online state, metrics, sample count |
| `GET /api/history?mac=&from=&to=&points=` | bucketed avg/min/max series per metric + range stats |
| `GET /api/events?limit=&mac=&since=` | alerts and events, newest first |
| `GET /api/export.csv?mac=&from=&to=` | raw samples as CSV (omit `mac` for all units) |
| `GET /api/health` | hub version, device/sample counts, database size, uptime |
| `POST /api/devices/<mac>` `{"label","room"}` | hub-side display name / room override |
| `DELETE /api/devices/<mac>` | forget a unit and its history |

## Security

The hub is meant for a **trusted local network**. It has no login, and anyone who can reach port
8080 can view the dashboard, post data or forget units. Don't forward its port to the internet.
For remote viewing, use a VPN into your LAN, for example WireGuard or Tailscale on the Pi.

## Compatibility

- **KUTSU 1.0 firmware** (XIAO C3/C5/C6/S3) in stationary mode: supported natively.
- **KUTSU Stationary DIY nodes** (v2 firmware) push to an elected *master* over mDNS on port 80.
  They don't look for the hub. Their records are accepted if they are sent to `/api/ingest`, so
  a future firmware update or a fixed master address can point them here.
- **Python:** written for 3.7+ (Raspberry Pi OS Buster and later, Windows 7 with 3.8). Tested so far on 3.12 (Windows 11).
