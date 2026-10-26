#!/usr/bin/env python3
"""
Fake KUTSU Sense device(s) for trying the hub without hardware.

It speaks the same protocol as the firmware in STATIONARY mode:
  1. broadcasts a UDP discovery probe and waits for the hub's answer
     (or uses --hub host:port directly),
  2. POSTs its latest reading to /api/live every 5 s,
  3. POSTs its "log" lines to /api/ingest?src=log in NDJSON batches.

    python3 simulate_device.py                     # one device, auto-discover the hub
    python3 simulate_device.py -n 3 --hub 127.0.0.1:8080
    python3 simulate_device.py --backfill-hours 24 # also upload a day of history first
"""
import argparse
import json
import math
import random
import socket
import time
import urllib.request

ROOMS = ["Workshop", "Office", "Storage"]


def discover(udp_port, timeout=6.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    s.settimeout(1.0)
    probe = json.dumps({"kutsu_discover": 1, "id": "simulator"}).encode()
    end = time.time() + timeout
    while time.time() < end:
        s.sendto(probe, ("255.255.255.255", udp_port))
        try:
            data, addr = s.recvfrom(2048)
        except socket.timeout:
            continue
        try:
            msg = json.loads(data.decode())
        except ValueError:
            continue
        if msg.get("kutsu_hub"):
            return "%s:%d" % (addr[0], int(msg.get("port", 8080)))
    return None


class FakeDevice:
    def __init__(self, i, kind):
        self.mac = "AA:BB:CC:00:00:%02X" % (0x10 + i)
        self.name = ["Otter", "Heron", "Lynx", "Badger", "Wren"][i % 5] + "-%02X" % (0x10 + i)
        self.room = ROOMS[i % len(ROOMS)]
        self.kind = kind if kind != "mixed" else ("diy" if i % 3 == 2 else "sensirion")
        self.hw = "XIAO ESP32-C3" if self.kind == "diy" else "XIAO ESP32-C6"
        self.boot = time.time() - random.randint(60, 3600)
        self.phase = random.random() * 6.28

    def record(self, ts):
        h = (ts % 86400) / 3600.0
        occ = max(0.0, math.sin((h - 8) / 10 * math.pi))      # "people in the room" during the day
        n = lambda a: random.gauss(0, a)
        r = {"v": 3, "id": self.name, "mac": self.mac, "hw": self.hw, "role": "slave", "room": self.room,
             "up": max(0, int(ts - self.boot)), "ts": int(ts), "lat": None, "lon": None, "loc_ts": None,
             "alarm": 0, "fault": 0}
        if self.kind == "diy":
            r.update(eco2=int(450 + 900 * occ + n(30)), tvoc=int(40 + 500 * occ + n(15)))
        else:
            r.update(co2=int(430 + 1100 * occ + 120 * math.sin(ts / 900 + self.phase) + n(15)),
                     voc=int(100 + 160 * occ + n(6)), nox=max(1, int(1 + 15 * occ + n(1))))
        r.update(temp=round(21 + 2.5 * occ + n(0.1), 1), hum=round(42 + 8 * math.sin(ts / 7200 + self.phase) + n(0.4), 1))
        sev = {}
        for k, (w, a) in {"co2": (1000, 2000), "eco2": (900, 2000), "tvoc": (660, 3000),
                          "voc": (250, 400), "nox": (20, 150)}.items():
            if k in r:
                sev[k] = 2 if r[k] >= a else 1 if r[k] >= w else 0
        sev["temp"] = 2 if r["temp"] > 60 else 1 if r["temp"] > 50 else 0
        sev["hum"] = 2 if (r["hum"] < 25 or r["hum"] > 70) else 1 if (r["hum"] < 30 or r["hum"] > 60) else 0
        for k, s in sev.items():
            r["s_" + k] = s
        r["sev"] = max(sev.values())
        r["alarm"] = 1 if r["sev"] == 2 else 0
        return r


def post(hub, path, body, ctype):
    req = urllib.request.Request("http://%s%s" % (hub, path), data=body, method="POST",
                                 headers={"Content-Type": ctype})
    with urllib.request.urlopen(req, timeout=5) as resp:
        return json.loads(resp.read().decode())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--hub", help="host:port of the hub (default: discover over UDP)")
    ap.add_argument("--udp-port", type=int, default=47800)
    ap.add_argument("-n", "--devices", type=int, default=1)
    ap.add_argument("--kind", choices=["sensirion", "diy", "mixed"], default="mixed")
    ap.add_argument("--backfill-hours", type=float, default=0, help="upload this much history first")
    ap.add_argument("--every", type=float, default=5.0, help="seconds between pushes")
    a = ap.parse_args()

    hub = a.hub or discover(a.udp_port)
    if not hub:
        raise SystemExit("no hub answered on UDP %d - start kutsu_hub.py or pass --hub host:port" % a.udp_port)
    print("hub:", hub)
    devs = [FakeDevice(i, a.kind) for i in range(a.devices)]

    if a.backfill_hours > 0:
        now = time.time()
        for d in devs:
            t, total = now - a.backfill_hours * 3600, 0
            while t < now:
                lines = []
                while t < now and len(lines) < 200:
                    lines.append(json.dumps(d.record(t)))
                    t += 10
                total += post(hub, "/api/ingest?src=log", ("\n".join(lines) + "\n").encode(), "application/x-ndjson")["stored"]
            print("%s: backfilled %d samples" % (d.name, total))

    pending = {d.mac: [] for d in devs}
    while True:
        t = time.time()
        for d in devs:
            rec = d.record(t)
            pending[d.mac].append(json.dumps(rec))
            try:
                post(hub, "/api/live", json.dumps(rec).encode(), "application/json")
                post(hub, "/api/ingest?src=log", ("\n".join(pending[d.mac]) + "\n").encode(), "application/x-ndjson")
                pending[d.mac] = []
            except OSError as e:
                print("%s: hub unreachable (%s); keeping %d lines" % (d.name, e, len(pending[d.mac])))
        time.sleep(a.every)


if __name__ == "__main__":
    main()
