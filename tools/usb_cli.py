#!/usr/bin/env python3
"""Talk to the firmware's USB console (see docs/usb-console.md).

  pip install pyserial
  python tools/usb_cli.py -p COM3 status
  python tools/usb_cli.py -p COM3 scan
  python tools/usb_cli.py -p COM3 mode sniff
  python tools/usb_cli.py -p COM3 clone a4c1382a5122

One command per invocation: the board answers with JSON lines and finishes with `#END`.
ESP_LOG output shares the same port, so pass --raw to see it instead of parsing.
"""

import argparse
import json
import sys
import time

try:
    import serial
except ImportError:
    sys.exit("pyserial missing: pip install pyserial")


def run(port, cmd, baud=115200, timeout=15.0, dtr=False, rts=False):
    sp = serial.Serial(port, baud, timeout=0.2)
    # USB-Serial/JTAG maps DTR/RTS onto BOOT/EN: leave them de-asserted so opening the
    # port does not reset the board (esptool does that on purpose, we do not).
    sp.dtr, sp.rts = dtr, rts
    try:
        sp.reset_input_buffer()
        sp.write((cmd + "\r\n").encode())
        lines, deadline = [], time.time() + timeout
        while time.time() < deadline:
            line = sp.readline().decode("utf-8", "replace").strip()
            if not line:
                continue
            if line == "#END":
                return lines
            if line.startswith("#ERR"):
                lines.append({"err": line[4:].strip()})
            elif line.startswith("#"):
                lines.append({"note": line[1:].strip()})
            elif line.startswith("{"):
                try:
                    lines.append(json.loads(line))
                except json.JSONDecodeError:
                    lines.append({"line": line})
            else:
                lines.append({"line": line})       # ESP_LOG noise from other tasks
        sys.exit("timeout: no #END (is the firmware built with usb_console.c?)")
    finally:
        sp.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", nargs="+", help="command, e.g. status / scan / mode sniff")
    ap.add_argument("-p", "--port", default="COM3")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--timeout", type=float, default=15.0)
    ap.add_argument("--raw", action="store_true", help="print lines instead of JSON")
    ap.add_argument("--dtr", action="store_true", help="assert DTR on open (may reset)")
    ap.add_argument("--rts", action="store_true", help="assert RTS on open (may reset)")
    a = ap.parse_args()

    out = run(a.port, " ".join(a.cmd), a.baud, a.timeout, a.dtr, a.rts)
    if a.raw:
        for o in out:
            print(o.get("line") or o.get("note") or o.get("err") or json.dumps(o, ensure_ascii=False))
    else:
        for o in out:
            print(json.dumps(o, ensure_ascii=False, indent=1) if len(out) == 1 else json.dumps(o, ensure_ascii=False))
    return 1 if any("err" in o for o in out) else 0


if __name__ == "__main__":
    sys.exit(main())
