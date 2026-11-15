"""BLE advertisement watcher for Windows (Realtek/Intel/any BT 4.0+ adapter).

Used to check that the board's replayed advertisement is on air:
  python tools/ble_watch.py -s 15                      # list nearby advertisers
  python tools/ble_watch.py -s 20 --addr a4c1382a5122  # follow one address
  python tools/ble_watch.py -s 20 --mfg 0215e2c56db5dffb48d2b060d0f5a71096e00001002a3b

--mfg is the manufacturer-data payload after the company id, exactly as the 31-byte
firmware raw minus the `02 01 06 1a ff 4c 00` prefix. Every matching device is
reported with address, RSSI, packet count and an EMA of the advertising interval.
"""

import argparse
import asyncio
import sys
import time

from bleak import BleakScanner

seen = {}  # address -> dict


def absorb(dev, adv):
    now = time.monotonic()
    d = seen.setdefault(dev.address, {
        "address": dev.address, "name": None, "rssi": 0, "pkts": 0,
        "itvl_ms": 0.0, "last": 0.0, "mfg": b"", "svc": b"", "uuids": (),
    })
    if d["pkts"]:
        gap = (now - d["last"]) * 1000.0
        if gap < 20000:                      # ignore long absences
            d["itvl_ms"] = gap if not d["itvl_ms"] else d["itvl_ms"] * 0.75 + gap * 0.25
    d["last"] = now
    d["pkts"] += 1
    d["rssi"] = adv.rssi
    if adv.local_name:
        d["name"] = adv.local_name
    if adv.manufacturer_data:
        # one 31-byte legacy advert carries a single manufacturer AD structure
        cid, payload = next(iter(adv.manufacturer_data.items()))
        d["mfg"] = cid.to_bytes(2, "little") + payload
    if adv.service_data:
        d["svc"] = next(iter(adv.service_data.values()))
    if adv.service_uuids:
        d["uuids"] = tuple(adv.service_uuids)


def fmt_mfg(mfg):
    if not mfg:
        return "-"
    s = mfg.hex()
    if s.startswith("004c0215"):              # company 0x004C + iBeacon subtype
        u = s[8:40]
        uuid = f"{u[0:8]}-{u[8:12]}-{u[12:16]}-{u[16:20]}-{u[20:]}"
        major, minor = int(s[40:44], 16), int(s[44:48], 16)
        return f"iBeacon {uuid} {major}/{minor} ref={int(s[48:50], 16) - 256}dBm"
    return s


def check_expected(exp_hex, d):
    """Match the payload segments we can see against the firmware's raw AD hex.
    Segments are length+type prefixed, so the flags byte and the AD order do not matter."""
    exp = exp_hex.lower()
    segs = []
    if d["name"]:
        nm = d["name"].encode("utf-8").hex()
        segs.append(("name", f"{len(nm) // 2 + 1:02x}09{nm}"))
    if d["mfg"]:
        segs.append(("mfg", f"{len(d['mfg']) + 1:02x}ff{d['mfg'].hex()}"))
    if d["svc"]:
        segs.append(("svc", f"{len(d['svc']) + 1:02x}16{d['svc'].hex()}"))
    if not segs:
        return None, segs
    return all(s in exp for _, s in segs), segs


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-s", "--seconds", type=float, default=15.0)
    ap.add_argument("--addr", help="only report this MAC (12 hex, no colons)")
    ap.add_argument("--mfg", help="only report devices whose manufacturer data matches this hex")
    ap.add_argument("--expect", help="firmware raw AD hex (as printed by `adv: clone on: ... raw=`) to compare against")
    ap.add_argument("--raw", action="store_true", help="print the full hex of every payload")
    ap.add_argument("--min-rssi", type=int, default=-127,
                    help="ignore advertisements weaker than this (dBm); the board sits next to the PC")
    ap.add_argument("--wait", action="store_true",
                    help="keep scanning until a match appears (or --seconds elapses)")
    ap.add_argument("--active", action="store_true",
                    help="send scan requests instead of staying passive (Windows default)")
    a = ap.parse_args()

    want_mfg = bytes.fromhex(a.mfg) if a.mfg else None
    want_addr = a.addr.replace(":", "").lower() if a.addr else None

    print(f"scanning {a.seconds:.0f}s on the local adapter ...", file=sys.stderr, flush=True)
    scanner = BleakScanner(detection_callback=absorb,
                           scanning_mode="active" if a.active else "passive")
    await scanner.start()

    def matches():
        rows = [d for d in seen.values() if d["pkts"] > 1 and d["rssi"] >= a.min_rssi]
        if want_addr:
            rows = [d for d in rows if d["address"].replace(":", "").lower() == want_addr]
        if want_mfg is not None:
            rows = [d for d in rows if want_mfg in d["mfg"]]
        return rows

    deadline = time.monotonic() + a.seconds
    if a.wait:
        while time.monotonic() < deadline and not matches():
            await asyncio.sleep(1.0)
    else:
        await asyncio.sleep(max(0.0, deadline - time.monotonic()))
    await scanner.stop()

    rows = matches()
    rows.sort(key=lambda d: d["pkts"], reverse=True)

    if not rows:
        print("no matching advertisements. board in sniff mode? press BOOT once "
              "(single click toggles sniff <-> clone) and scan again.")
        return 1

    print(f"{'address':18} {'rssi':>5} {'pkts':>5} {'itvl':>7}  payload")
    for d in rows:
        print(f"{d['address']:18} {d['rssi']:>4}dB {d['pkts']:>5} {d['itvl_ms']:>6.0f}ms  "
              f"{d['name'] or '-'} | {fmt_mfg(d['mfg'])}")
        if a.raw and d["svc"]:
            print(f"{'':18} service data: {d['svc'].hex()}")
        if a.expect:
            ok, segs = check_expected(a.expect, d)
            if ok is None:
                print(f"{'':18} no comparable segment (name/mfg empty)")
            elif ok:
                print(f"{'':18} MATCH  every segment of the expected raw is on air: "
                      + ", ".join(f"{n} {s}" for n, s in segs))
            else:
                missing = [f"{n} {s}" for n, s in segs if s not in a.expect.lower()]
                print(f"{'':18} MISMATCH  expected raw does not contain: " + ", ".join(missing))
    return 0


sys.exit(asyncio.run(main()))
