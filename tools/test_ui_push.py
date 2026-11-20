#!/usr/bin/env python3
"""Checks for tools/ui_push.py -- the pieces worth guarding, no board and no network.

    python tools/test_ui_push.py      (also under -O: bare asserts would vanish there)

"""

import contextlib
import io
import sys

import ui_push

checks = 0


def check(cond, what):
    global checks
    if not cond:                                   # not `assert`: -O must not turn this green
        raise SystemExit("FAIL: " + what)
    checks += 1


# The 1..64 KB bound is inclusive at both ends: the board 400s below, 413s above.
for n in (1023, 0, 65537, 1 << 20):
    check(not ui_push.size_ok(n), "%d must be refused" % n)
for n in (ui_push.MIN_BYTES, ui_push.MAX_BYTES):
    check(ui_push.size_ok(n), "%d must be accepted" % n)

# ok:false exits non-zero and still prints the reason; junk and truncated bodies become ok:false.
with contextlib.redirect_stdout(io.StringIO()):
    try:
        ui_push.report({"ok": False, "err": "not gzip"})
    except SystemExit as e:
        check(e.code == 1, "ok:false must exit 1")
    else:
        check(False, "ok:false must not return")
    ui_push.report({"ok": True, "source": "embedded"})          # ok:true returns normally

check(ui_push.as_json('{"ok":false,"err":"not gzip"}', 400)["err"] == "not gzip", "reason kept")
check(ui_push.as_json('{"ok":true,"source":"fs"}', 200)["source"] == "fs", "body parsed")
check(ui_push.as_json("", 200) == {}, "empty body is an empty object")
junk = ui_push.as_json("<html>500 Internal Server Error</html>", 500)
check(junk["ok"] is False and junk["err"].startswith("HTTP 500: "), "non-JSON error body")
check(len(junk["err"]) < 100, "non-JSON error body is truncated")
check(ui_push.as_json('["not","an","object"]', 200)["ok"] is False, "non-object JSON refused")

# The argument guard answers before any socket is opened -- provable offline.
argv = sys.argv
for bad in (["--pass", "x", "--status", "x.html"],
            ["--pass", "x", "--revert", "x.html"],
            ["--pass", "x", "--status", "--revert"]):
    sys.argv = ["ui_push.py"] + bad
    err = io.StringIO()
    try:
        with contextlib.redirect_stderr(err):
            ui_push.main()
        code = None
    except SystemExit as e:
        code = e.code
    check(code == 2 and "error:" in err.getvalue(), "%s must exit 2" % " ".join(bad))
sys.argv = argv

print("ok  ui_push: %d checks passed" % checks)