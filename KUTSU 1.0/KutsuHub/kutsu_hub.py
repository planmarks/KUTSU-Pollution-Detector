#!/usr/bin/env python3
"""
KUTSU Hub - local collector + dashboard for KUTSU Sense devices in STATIONARY mode.

Run it on any always-on machine on the same LAN as the devices (Raspberry Pi,
an old Windows PC, a NAS...). Devices find it automatically over UDP broadcast,
push their records to it, and it stores them in a local SQLite file and serves a
dashboard at  http://<this-machine>:8080/

    python3 kutsu_hub.py                 # defaults: port 8080, ./kutsu_hub.db
    python3 kutsu_hub.py --help          # all options

Standard library only (Python 3.7+) - nothing to pip install, no internet needed.

Protocol (shared with the firmware, see README.md):
  UDP 47800   device -> broadcast  {"kutsu_discover":1,"id":..,"mac":..}
              hub    -> reply/beacon {"kutsu_hub":1,"v":1,"port":8080,"name":..,"time":<epoch>}
  POST /api/live     one record (latest reading; not stored; drives alerts/online state)
  POST /api/ingest   records to STORE: a JSON object, a JSON array, or NDJSON lines
                     (the device streams its SD/flash log tail here)
"""

import argparse
import csv
import io
import json
import logging
import math
import os
import re
import socket
import sqlite3
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

HUB_VERSION = "1.0"
PROTO_VERSION = 1

# Metric keys from the device data contract (DATA_CONTRACT.md), in display order.
METRICS = ["co2", "eco2", "tvoc", "voc", "nox", "temp", "hum", "gas"]
METRIC_LABEL = {"co2": "CO2", "eco2": "eCO2", "tvoc": "TVOC", "voc": "VOC index",
                "nox": "NOx index", "temp": "Temperature", "hum": "Humidity", "gas": "Gas (MQ-2)"}
METRIC_UNIT = {"co2": "ppm", "eco2": "ppm", "tvoc": "ppb", "voc": "", "nox": "",
               "temp": "°C", "hum": "%RH", "gas": "raw"}

MAX_BODY = 4 * 1024 * 1024          # largest accepted request body
MIN_SANE_EPOCH = 1600000000         # anything older is "device clock not set"

log = logging.getLogger("kutsu-hub")

SCHEMA = """
CREATE TABLE IF NOT EXISTS devices(
  mac           TEXT PRIMARY KEY,
  name          TEXT,      -- name the device reports (record "id")
  label         TEXT,      -- hub-side display name override ("" = use name)
  room          TEXT,      -- room the device reports
  room_override TEXT,      -- hub-side room override ("" = use room)
  hw TEXT, fw TEXT, ip TEXT,
  first_seen    INTEGER,
  last_seen     INTEGER,   -- hub time of the last record received
  last_record   TEXT,      -- most recent record JSON
  metrics       TEXT       -- JSON list of metric keys this device reports
);
CREATE TABLE IF NOT EXISTS samples(
  mac TEXT NOT NULL, ts INTEGER NOT NULL, up INTEGER, sev INTEGER, alarm INTEGER,
  co2 REAL, eco2 REAL, tvoc REAL, voc REAL, nox REAL, temp REAL, hum REAL, gas REAL,
  UNIQUE(mac, ts, up)
);
CREATE INDEX IF NOT EXISTS samples_mac_ts ON samples(mac, ts);
CREATE TABLE IF NOT EXISTS events(
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  mac TEXT, ts INTEGER, level TEXT, metric TEXT, value REAL, text TEXT
);
CREATE INDEX IF NOT EXISTS events_ts ON events(ts);
"""


def now_s():
    return int(time.time())


def num(v):
    """JSON value -> float or None (rejects bools/strings/NaN)."""
    if isinstance(v, bool) or not isinstance(v, (int, float)):
        return None
    if isinstance(v, float) and (math.isnan(v) or math.isinf(v)):
        return None
    return float(v)


def parse_records(body):
    """Accept a JSON object, a JSON array, or NDJSON. Returns a list of dicts."""
    text = body.decode("utf-8", "replace").strip()
    if not text:
        return []
    try:
        obj = json.loads(text)
        if isinstance(obj, dict):
            return [obj]
        if isinstance(obj, list):
            return [o for o in obj if isinstance(o, dict)]
    except ValueError:
        pass
    out = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except ValueError:
            continue                      # a torn/garbled log line: skip it
        if isinstance(o, dict):
            out.append(o)
    return out


class Store:
    """SQLite store + in-memory live state. One connection, guarded by a lock."""

    def __init__(self, path, store_every, offline_after):
        self.path = path
        self.store_every = store_every
        self.offline_after = offline_after
        self.lock = threading.RLock()
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.row_factory = sqlite3.Row
        with self.lock:
            self.db.execute("PRAGMA journal_mode=WAL")
            self.db.execute("PRAGMA synchronous=NORMAL")
            self.db.executescript(SCHEMA)
            self.db.commit()
        self.last_stored = {}      # mac -> (ts, sev) of the last stored sample
        self.prev_sev = {}         # mac -> {metric: sev} for alert transitions
        self.online = {}           # mac -> bool (for online/offline events)
        self.started = now_s()

    # ---- devices -------------------------------------------------------------
    def _touch_device(self, rec, ip, rx, update_latest):
        mac = str(rec.get("mac") or rec.get("id") or "").strip()
        if not mac:
            return None
        metrics = [m for m in METRICS if m in rec]
        row = self.db.execute("SELECT mac, metrics FROM devices WHERE mac=?", (mac,)).fetchone()
        name = str(rec.get("id") or mac)
        room = str(rec.get("room") or "")
        hw = str(rec.get("hw") or "")
        if row is None:
            self.db.execute(
                "INSERT INTO devices(mac,name,label,room,room_override,hw,fw,ip,first_seen,last_seen,last_record,metrics)"
                " VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                (mac, name, "", room, "", hw, "", ip or "", rx, rx if update_latest else None,
                 json.dumps(rec) if update_latest else None, json.dumps(metrics)))
            self.add_event(mac, rx, "info", None, None, "New device %s joined" % name)
        else:
            known = json.loads(row["metrics"] or "[]")
            merged = [m for m in METRICS if m in known or m in metrics]
            if update_latest:
                self.db.execute(
                    "UPDATE devices SET name=?, room=?, hw=?, ip=COALESCE(NULLIF(?,''),ip), last_seen=?,"
                    " last_record=?, metrics=? WHERE mac=?",
                    (name, room, hw, ip or "", rx, json.dumps(rec), json.dumps(merged), mac))
            elif merged != known:
                self.db.execute("UPDATE devices SET metrics=? WHERE mac=?", (json.dumps(merged), mac))
        return mac

    def _display_name(self, mac):
        r = self.db.execute("SELECT name,label FROM devices WHERE mac=?", (mac,)).fetchone()
        if not r:
            return mac
        return r["label"] or r["name"] or mac

    # ---- events --------------------------------------------------------------
    def add_event(self, mac, ts, level, metric, value, text):
        self.db.execute("INSERT INTO events(mac,ts,level,metric,value,text) VALUES(?,?,?,?,?,?)",
                        (mac, ts, level, metric, value, text))

    def _detect_alerts(self, mac, rec, rx):
        """Per-metric severity rises -> warning/alert events; drops to 0 -> cleared."""
        prev = self.prev_sev.setdefault(mac, {})
        name = self._display_name(mac)
        for m in METRICS:
            key = "s_" + m
            if key not in rec:
                continue
            s = rec.get(key)
            if not isinstance(s, int):
                continue
            p = prev.get(m, 0)
            if s > p:
                lvl = "alert" if s >= 2 else "warning"
                v = num(rec.get(m))
                txt = "%s: %s %s %s" % (name, METRIC_LABEL.get(m, m),
                                        ("%g" % v) if v is not None else "?", METRIC_UNIT.get(m, ""))
                self.add_event(mac, rx, lvl, m, v, txt.strip())
            elif s == 0 and p > 0:
                self.add_event(mac, rx, "cleared", m, num(rec.get(m)),
                               "%s: %s back to normal" % (name, METRIC_LABEL.get(m, m)))
            prev[m] = s

    # ---- samples -------------------------------------------------------------
    def _store_sample(self, mac, rec, rx, force=False):
        ts = rec.get("ts")
        if not isinstance(ts, int) or isinstance(ts, bool) or ts < MIN_SANE_EPOCH or ts > rx + 86400:
            ts = rx                                   # device clock unset: stamp with hub time
        sev = rec.get("sev") if isinstance(rec.get("sev"), int) else 0
        last = self.last_stored.get(mac)
        if (not force and last is not None and self.store_every > 0
                and 0 <= ts - last[0] < self.store_every and sev == last[1]):
            return False                              # thinning: keep 1 per store_every s (+ every sev change)
        up = rec.get("up") if isinstance(rec.get("up"), int) else None
        vals = [num(rec.get(m)) for m in METRICS]
        cur = self.db.execute(
            "INSERT OR IGNORE INTO samples(mac,ts,up,sev,alarm,%s) VALUES(?,?,?,?,?,%s)"
            % (",".join(METRICS), ",".join("?" * len(METRICS))),
            [mac, ts, up, sev, 1 if rec.get("alarm") else 0] + vals)
        if cur.rowcount:
            if last is None or ts >= last[0]:
                self.last_stored[mac] = (ts, sev)
            return True
        return False

    def ingest(self, records, ip, live, from_log=False):
        """Store records (and, for live/legacy pushes, update latest state + alerts).

        live=True     /api/live: latest reading only, never stored.
        from_log=True a device log batch (?src=log): stored only - it may be old data.
        otherwise     a legacy single-record push (KUTSU Stationary): live AND stored.
        """
        rx = now_s()
        stored = 0
        with self.lock:
            samples = [r for r in records if "ev" not in r]
            events = [r for r in records if "ev" in r]
            for i, rec in enumerate(samples):
                newest = (i == len(samples) - 1)
                is_live = live or (not from_log and len(samples) == 1)
                mac = self._touch_device(rec, ip, rx, update_latest=is_live and newest)
                if not mac:
                    continue
                if is_live and newest:
                    self._detect_alerts(mac, rec, rx)
                    self._mark_online(mac, rx, ip)
                if not live and self._store_sample(mac, rec, rx):
                    stored += 1
            for ev in events:
                mac = str(ev.get("mac") or "")
                if not mac:
                    continue                          # device log events carry only "id"; ignore
                self.add_event(mac, rx, "info", None, None, "%s: %s" % (ev.get("id", mac), ev.get("ev")))
            self.db.commit()
        return stored

    def _mark_online(self, mac, rx, ip=None):
        if not self.online.get(mac, False):
            if mac in self.online:                    # known device coming back
                self.add_event(mac, rx, "info", None, None, "%s is back online" % self._display_name(mac))
            log.info("device %s (%s) online from %s", self._display_name(mac), mac, ip or "?")
            self.online[mac] = True

    def check_offline(self):
        t = now_s()
        with self.lock:
            for r in self.db.execute("SELECT mac,last_seen FROM devices").fetchall():
                mac = r["mac"]
                on = (t - (r["last_seen"] or 0)) < self.offline_after
                was = self.online.get(mac)
                if was is None:
                    self.online[mac] = on
                elif was and not on:
                    self.online[mac] = False
                    self.add_event(mac, t, "info", None, None,
                                   "%s went offline" % self._display_name(mac))
                    log.info("device %s (%s) offline: nothing for %ds", self._display_name(mac), mac,
                             self.offline_after)
            self.db.commit()

    def prune(self, retention_days):
        if retention_days <= 0:
            return
        cutoff = now_s() - retention_days * 86400
        with self.lock:
            n = self.db.execute("DELETE FROM samples WHERE ts < ?", (cutoff,)).rowcount
            self.db.execute("DELETE FROM events WHERE ts < ?", (cutoff,))
            self.db.commit()
        if n:
            log.info("retention: pruned %d samples older than %d days", n, retention_days)

    # ---- queries -------------------------------------------------------------
    def devices(self):
        t = now_s()
        out = []
        with self.lock:
            rows = self.db.execute("SELECT * FROM devices ORDER BY COALESCE(NULLIF(room_override,''),room), name").fetchall()
            counts = dict(self.db.execute("SELECT mac, COUNT(*) FROM samples GROUP BY mac").fetchall())
        for r in rows:
            last = json.loads(r["last_record"]) if r["last_record"] else None
            age = (t - r["last_seen"]) if r["last_seen"] else None    # None = never seen live
            out.append({
                "mac": r["mac"],
                "name": r["label"] or r["name"] or r["mac"],
                "device_name": r["name"], "label": r["label"] or "",
                "room": r["room_override"] or r["room"] or "",
                "device_room": r["room"] or "", "room_override": r["room_override"] or "",
                "hw": r["hw"] or "", "ip": r["ip"] or "",
                "first_seen": r["first_seen"], "last_seen": r["last_seen"],
                "age": age, "online": age is not None and age < self.offline_after,
                "metrics": json.loads(r["metrics"] or "[]"),
                "samples": counts.get(r["mac"], 0),
                "last": last,
            })
        return out

    def update_device(self, mac, label=None, room=None):
        with self.lock:
            if label is not None:
                self.db.execute("UPDATE devices SET label=? WHERE mac=?", (label.strip()[:40], mac))
            if room is not None:
                self.db.execute("UPDATE devices SET room_override=? WHERE mac=?", (room.strip()[:40], mac))
            self.db.commit()
            return self.db.execute("SELECT 1 FROM devices WHERE mac=?", (mac,)).fetchone() is not None

    def forget_device(self, mac):
        with self.lock:
            self.db.execute("DELETE FROM samples WHERE mac=?", (mac,))
            self.db.execute("DELETE FROM events WHERE mac=?", (mac,))
            n = self.db.execute("DELETE FROM devices WHERE mac=?", (mac,)).rowcount
            self.db.commit()
        self.last_stored.pop(mac, None)
        self.prev_sev.pop(mac, None)
        self.online.pop(mac, None)
        return n > 0

    def history(self, mac, t0, t1, points):
        span = max(1, t1 - t0)
        bucket = max(1, int(math.ceil(span / float(max(10, min(points, 2000))))))
        cols = []
        for m in METRICS:
            cols += ["AVG(%s)" % m, "MIN(%s)" % m, "MAX(%s)" % m]
        sql = ("SELECT (ts/?)*? AS b, %s, MAX(sev) FROM samples WHERE mac=? AND ts>=? AND ts<=?"
               " GROUP BY b ORDER BY b" % ",".join(cols))
        with self.lock:
            rows = self.db.execute(sql, (bucket, bucket, mac, t0, t1)).fetchall()
            stats = self.db.execute(
                "SELECT COUNT(*), %s FROM samples WHERE mac=? AND ts>=? AND ts<=?"
                % ",".join("AVG(%s),MIN(%s),MAX(%s)" % (m, m, m) for m in METRICS),
                (mac, t0, t1)).fetchone()
        res = {"mac": mac, "from": t0, "to": t1, "bucket": bucket, "t": [], "sev": [],
               "avg": {}, "min": {}, "max": {}, "stats": {}, "count": stats[0]}
        for m in METRICS:
            res["avg"][m], res["min"][m], res["max"][m] = [], [], []
        for row in rows:
            res["t"].append(row[0])
            for i, m in enumerate(METRICS):
                res["avg"][m].append(row[1 + 3 * i])
                res["min"][m].append(row[2 + 3 * i])
                res["max"][m].append(row[3 + 3 * i])
            res["sev"].append(row[1 + 3 * len(METRICS)])
        present = []
        for i, m in enumerate(METRICS):
            a, lo, hi = stats[1 + 3 * i], stats[2 + 3 * i], stats[3 + 3 * i]
            if a is not None:
                present.append(m)
                res["stats"][m] = {"avg": a, "min": lo, "max": hi}
        for m in METRICS:                              # drop all-empty series
            if m not in present:
                del res["avg"][m], res["min"][m], res["max"][m]
        res["metrics"] = present
        return res

    def events(self, limit, mac=None, since=0):
        q = "SELECT * FROM events WHERE ts>=?"
        args = [since]
        if mac:
            q += " AND mac=?"
            args.append(mac)
        q += " ORDER BY id DESC LIMIT ?"
        args.append(limit)
        with self.lock:
            return [dict(r) for r in self.db.execute(q, args).fetchall()]

    def export_rows(self, mac, t0, t1):
        """Generator of CSV-ready rows (fetched in pages so huge ranges stay cheap)."""
        names = {d["mac"]: d["name"] for d in self.devices()}
        last_rowid = 0
        while True:
            q = ("SELECT rowid,* FROM samples WHERE rowid>? AND ts>=? AND ts<=?"
                 + (" AND mac=?" if mac else "") + " ORDER BY rowid LIMIT 5000")
            args = [last_rowid, t0, t1] + ([mac] if mac else [])
            with self.lock:
                rows = self.db.execute(q, args).fetchall()
            if not rows:
                return
            for r in rows:
                last_rowid = r["rowid"]
                yield ([time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(r["ts"])), r["ts"],
                        names.get(r["mac"], r["mac"]), r["mac"], r["up"], r["sev"], r["alarm"]]
                       + ["" if r[m] is None else r[m] for m in METRICS])

    def health(self):
        with self.lock:
            nd = self.db.execute("SELECT COUNT(*) FROM devices").fetchone()[0]
            ns = self.db.execute("SELECT COUNT(*) FROM samples").fetchone()[0]
        size = 0
        for suffix in ("", "-wal"):
            try:
                size += os.path.getsize(self.path + suffix)
            except OSError:
                pass
        return {"ok": 1, "hub": HUB_VERSION, "devices": nd, "samples": ns,
                "db_bytes": size, "uptime": now_s() - self.started, "time": now_s()}


# ---------------------------------------------------------------------------
# UDP discovery: answer device probes + broadcast a periodic beacon
# ---------------------------------------------------------------------------
def broadcast_targets():
    """Limited broadcast plus each LAN interface's /24 directed broadcast.

    With several adapters (VPN, Hyper-V, Bluetooth PAN, link-local...) Windows sends
    255.255.255.255 out of only one of them, which may not be the Wi-Fi/Ethernet the
    devices are on; a directed broadcast (e.g. 192.168.1.255) always leaves the right one.
    """
    targets = ["255.255.255.255"]
    for ip in local_ips():
        if ip.startswith("169.254."):
            continue                                  # link-local: never where devices are
        parts = ip.split(".")
        if len(parts) == 4:
            targets.append(".".join(parts[:3] + ["255"]))
    return targets


def discovery_loop(udp_port, http_port, name, beacon_every, stop):
    def beacon():
        return json.dumps({"kutsu_hub": 1, "v": PROTO_VERSION, "port": http_port,
                           "name": name, "time": now_s()}).encode()

    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):        # Windows: a 2nd hub must fail, not share
        s.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        s.bind(("", udp_port))
    except OSError as e:
        log.error("discovery: cannot bind UDP %d (%s). Is another KUTSU Hub already running on this "
                  "machine? Devices will not find this one automatically.", udp_port, e)
        return
    s.settimeout(1.0)
    mine = set(local_ips())
    others = set()
    log.info("discovery: listening on UDP %d, beacon every %ds to %s",
             udp_port, beacon_every, ", ".join(broadcast_targets()))
    next_beacon = 0.0
    while not stop.is_set():
        t = time.time()
        if beacon_every > 0 and t >= next_beacon:
            for dst in broadcast_targets():
                try:
                    s.sendto(beacon(), (dst, udp_port))
                except OSError as e:
                    log.debug("beacon to %s failed: %s", dst, e)
            next_beacon = t + beacon_every
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        except OSError:
            continue
        try:
            msg = json.loads(data.decode("utf-8", "replace"))
        except ValueError:
            continue
        if not isinstance(msg, dict):
            continue
        if msg.get("kutsu_discover"):
            try:
                s.sendto(beacon(), addr)
                log.info("discovery: answered %s (%s) -> port %d", msg.get("id") or "?", addr[0], http_port)
            except OSError:
                pass
        elif msg.get("kutsu_hub"):
            key = (addr[0], msg.get("port"))
            if (addr[0] not in mine or msg.get("port") != http_port) and key not in others:
                others.add(key)                       # devices pick whichever hub answers first
                log.warning("another KUTSU Hub is running at %s:%s (\"%s\"). Devices may connect to "
                            "it instead of this one. Stop it, or pin devices with `hub <ip:port>`.",
                            addr[0], msg.get("port"), msg.get("name", "?"))


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
STATIC_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "static")
CTYPES = {".html": "text/html; charset=utf-8", ".js": "application/javascript; charset=utf-8",
          ".css": "text/css; charset=utf-8", ".svg": "image/svg+xml", ".png": "image/png",
          ".ico": "image/x-icon", ".json": "application/json"}
MAC_PATH = re.compile(r"^/api/devices/([^/]+)$")


def make_handler(store):
    class Handler(BaseHTTPRequestHandler):
        server_version = "KutsuHub/" + HUB_VERSION
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):     # route http.server's access log to debug
            log.debug("%s %s", self.client_address[0], fmt % args)

        # ---- helpers ----
        def _send(self, code, body, ctype="application/json", extra=None):
            if isinstance(body, (dict, list)):
                body = json.dumps(body, separators=(",", ":"))
            if isinstance(body, str):
                body = body.encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            for k, v in (extra or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _body(self):
            n = int(self.headers.get("Content-Length") or 0)
            if n < 0 or n > MAX_BODY:
                raise ValueError("body too large")
            return self.rfile.read(n) if n else b""

        def _q(self):
            u = urlparse(self.path)
            return u.path, {k: v[-1] for k, v in parse_qs(u.query).items()}

        def _range(self, q):
            t1 = int(q.get("to") or now_s())
            t0 = int(q.get("from") or (t1 - 3600))
            return t0, t1

        # ---- GET ----
        def do_GET(self):
            path, q = self._q()
            try:
                if path == "/api/devices":
                    return self._send(200, store.devices())
                if path == "/api/history":
                    if not q.get("mac"):
                        return self._send(400, {"error": "mac required"})
                    t0, t1 = self._range(q)
                    return self._send(200, store.history(q["mac"], t0, t1, int(q.get("points") or 400)))
                if path == "/api/events":
                    return self._send(200, store.events(min(int(q.get("limit") or 100), 1000),
                                                        q.get("mac"), int(q.get("since") or 0)))
                if path == "/api/health":
                    return self._send(200, store.health())
                if path in ("/api/export.csv", "/api/export"):
                    return self._export(q)
                return self._static(path)
            except (ValueError, KeyError) as e:
                return self._send(400, {"error": str(e)})

        do_HEAD = do_GET

        def _export(self, q):
            t0 = int(q.get("from") or 0)
            t1 = int(q.get("to") or now_s())
            mac = q.get("mac") or None
            fname = "kutsu_%s_%s.csv" % ((mac or "all").replace(":", ""),
                                          time.strftime("%Y%m%d-%H%M", time.localtime(t1)))
            self.send_response(200)
            self.send_header("Content-Type", "text/csv; charset=utf-8")
            self.send_header("Content-Disposition", 'attachment; filename="%s"' % fname)
            self.send_header("Connection", "close")    # streamed: no Content-Length
            self.end_headers()
            self.close_connection = True
            buf = io.StringIO()
            w = csv.writer(buf)
            w.writerow(["time_local", "ts", "device", "mac", "uptime_s", "sev", "alarm"] + METRICS)
            for i, row in enumerate(store.export_rows(mac, t0, t1)):
                w.writerow(row)
                if i % 500 == 499:
                    self.wfile.write(buf.getvalue().encode("utf-8"))
                    buf.seek(0)
                    buf.truncate()
            self.wfile.write(buf.getvalue().encode("utf-8"))

        def _static(self, path):
            if path in ("", "/"):
                path = "/index.html"
            rel = unquote(path).lstrip("/").replace("\\", "/")
            full = os.path.normpath(os.path.join(STATIC_DIR, rel))
            if not full.startswith(STATIC_DIR + os.sep) or not os.path.isfile(full):
                return self._send(404, {"error": "not found"})
            with open(full, "rb") as f:
                data = f.read()
            return self._send(200, data, CTYPES.get(os.path.splitext(full)[1], "application/octet-stream"))

        # ---- POST / DELETE ----
        def do_POST(self):
            path, q = self._q()
            try:
                body = self._body()
            except ValueError as e:
                return self._send(413, {"error": str(e)})
            ip = self.client_address[0]
            if path in ("/api/ingest", "/api/live", "/api/nodes/register"):
                recs = parse_records(body)
                stored = store.ingest(recs, ip, live=(path == "/api/live"), from_log=(q.get("src") == "log"))
                return self._send(200, {"ok": 1, "received": len(recs), "stored": stored, "time": now_s()})
            m = MAC_PATH.match(path)
            if m:
                try:
                    d = json.loads(body.decode("utf-8") or "{}")
                except ValueError:
                    return self._send(400, {"error": "bad json"})
                ok = store.update_device(unquote(m.group(1)), d.get("label"), d.get("room"))
                return self._send(200 if ok else 404, {"ok": 1 if ok else 0})
            return self._send(404, {"error": "not found"})

        def do_DELETE(self):
            path, _ = self._q()
            m = MAC_PATH.match(path)
            if not m:
                return self._send(404, {"error": "not found"})
            ok = store.forget_device(unquote(m.group(1)))
            return self._send(200 if ok else 404, {"ok": 1 if ok else 0})

    return Handler


def housekeeping(store, retention_days, stop):
    last_prune = 0.0
    while not stop.wait(5):
        try:
            store.check_offline()
            if time.time() - last_prune > 3600:
                store.prune(retention_days)
                last_prune = time.time()
        except Exception:                                # never let housekeeping die
            log.exception("housekeeping")


def local_ips():
    ips = set()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("10.255.255.255", 1))                 # no packet sent; picks the LAN interface
        ips.add(s.getsockname()[0])
        s.close()
    except OSError:
        pass
    try:
        for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
            ips.add(info[4][0])
    except OSError:
        pass
    return sorted(i for i in ips if not i.startswith("127."))


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description="KUTSU Hub: collect + display KUTSU Sense data on your LAN.")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default all interfaces)")
    ap.add_argument("--port", type=int, default=8080, help="HTTP port for the dashboard + device API (default 8080)")
    ap.add_argument("--db", default=os.path.join(here, "kutsu_hub.db"), help="SQLite database file")
    ap.add_argument("--udp-port", type=int, default=47800, help="UDP discovery port (must match the firmware)")
    ap.add_argument("--no-discovery", action="store_true", help="don't answer/broadcast UDP discovery")
    ap.add_argument("--beacon-every", type=int, default=10, help="discovery beacon period in s (0 = only answer probes)")
    ap.add_argument("--store-every", type=int, default=10,
                    help="store at most one sample per device every N s, plus every severity change (0 = store all)")
    ap.add_argument("--retention-days", type=int, default=365, help="delete samples older than this (0 = keep forever)")
    ap.add_argument("--offline-after", type=int, default=30, help="mark a device offline after N s of silence")
    ap.add_argument("--name", default="KUTSU Hub", help="hub name announced to devices")
    ap.add_argument("-v", "--verbose", action="store_true", help="debug logging")
    a = ap.parse_args()

    logging.basicConfig(level=logging.DEBUG if a.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(message)s", datefmt="%H:%M:%S")
    store = Store(a.db, a.store_every, a.offline_after)
    stop = threading.Event()
    threading.Thread(target=housekeeping, args=(store, a.retention_days, stop), daemon=True).start()
    if not a.no_discovery:
        threading.Thread(target=discovery_loop, args=(a.udp_port, a.port, a.name, a.beacon_every, stop),
                         daemon=True).start()

    if os.name == "nt":
        ThreadingHTTPServer.allow_reuse_address = False   # Windows SO_REUSEADDR = silent port sharing
    try:
        srv = ThreadingHTTPServer((a.host, a.port), make_handler(store))
    except OSError as e:
        log.error("cannot listen on %s:%d - %s (is another hub running? try --port)", a.host, a.port, e)
        sys.exit(1)
    srv.daemon_threads = True
    log.info("KUTSU Hub %s - database %s", HUB_VERSION, a.db)
    for ip in local_ips() or ["localhost"]:
        log.info("dashboard: http://%s:%d/", ip, a.port)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        srv.server_close()
        log.info("stopped")


if __name__ == "__main__":
    main()
