#!/usr/bin/env python3
"""Replace the SPA on a running board without reflashing it.

    python tools/ui_push.py --pass <AP password> main/www/index.html
    python tools/ui_push.py --pass <AP password> --status
    python tools/ui_push.py --pass <AP password> --revert

The file is gzipped here (Windows has no gzip CLI) because the device never inflates it.
"""

import argparse
import gzip
import http.client
import json
import pathlib
import sys
import urllib.error
import urllib.request

MAX_BYTES = 64 * 1024
MIN_BYTES = 1024


def size_ok(n):
    """The device refuses anything outside this inclusive range (400 / 413)."""
    return MIN_BYTES <= n <= MAX_BYTES


def as_json(raw, http):
    try:
        return json.loads(raw or "{}")
    except ValueError:
        return {"ok": False, "err": "HTTP %s: %r" % (http, raw[:80])}


def call(base, path, method="GET", body=None, token=None, ctype="application/json"):
    req = urllib.request.Request(base + path, method=method, data=body)
    if body is not None:
        req.add_header("Content-Type", ctype)
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        with urllib.request.urlopen(req, timeout=45) as r:
            return as_json(r.read().decode("utf-8", "replace"), r.status)
    except urllib.error.HTTPError as e:            # a refusal still carries the JSON reason
        return as_json(e.read().decode("utf-8", "replace"), e.code)
    except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
        # nothing listening, bad host, timeout -- or a body cut short mid-response
        sys.exit("cannot reach %s: %s" % (base, getattr(e, "reason", e)))


def source(state):
    """One line for `GET /api/ui`: only an uploaded copy has a size and an ETag."""
    if not state.get("ok"):
        return "error: " + str(state.get("err") or state)
    if state.get("source") == "fs":
        return "fs, %s bytes, etag %s" % (state.get("size"), state.get("etag"))
    return "embedded"


def report(out):
    print(json.dumps(out, indent=2))
    if not out.get("ok"):
        sys.exit(1)                                # a server refusal must not look like success


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file", nargs="?", help="index.html to upload")
    ap.add_argument("--host", default="192.168.4.1")
    ap.add_argument("--pass", dest="password", required=True, help="SoftAP password or API token")
    how = ap.add_mutually_exclusive_group()
    how.add_argument("--revert", action="store_true", help="drop the uploaded UI, back to the firmware copy")
    how.add_argument("--status", action="store_true", help="report which copy is being served")
    args = ap.parse_args()
    if args.file and (args.revert or args.status):
        ap.error("--status and --revert take no file: drop it, or drop the flag")

    base = "http://" + args.host
    sess = call(base, "/api/session", "POST", json.dumps({"password": args.password}).encode())
    token = sess.get("token")
    if not token:
        sys.exit(base + " refused the session: " + str(sess.get("err") or sess))

    if args.status:
        report(call(base, "/api/ui", token=token))
        return
    if args.revert:
        report(call(base, "/api/ui", "DELETE", token=token))
        return
    if not args.file:
        sys.exit("nothing to do: pass a file, --status or --revert")

    path = pathlib.Path(args.file)
    if not path.is_file():
        sys.exit("no such file: " + args.file)
    raw = path.read_bytes()
    blob = gzip.compress(raw, 9)
    print("raw %d -> gzip %d bytes" % (len(raw), len(blob)))
    if not size_ok(len(blob)):
        sys.exit("refusing: gzip must be %d..%d bytes" % (MIN_BYTES, MAX_BYTES))

    print("source before: " + source(call(base, "/api/ui", token=token)))
    report(call(base, "/api/ui", "POST", blob, token=token, ctype="application/gzip"))
    print("source after:  " + source(call(base, "/api/ui", token=token)))
    print("hard-reload the page (Ctrl-F5) to pick it up")


if __name__ == "__main__":
    main()