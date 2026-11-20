# USB console

The board exposes the same read/control surface as the HTTP API **over the USB cable**, with
no network involved. That is the only way to drive a board whose SoftAP you cannot join —
joining `BeaconGW-xxxx` costs the host its own Wi-Fi, and on a remote machine that means
losing the session you are working through.

The console shares the native USB-Serial/JTAG port with `esptool`/`idf.py monitor`, so only
one of them can own the port at a time.

## Protocol

Line-oriented, ASCII, `\r` or `\n` terminated. One command per line:

```
status
{...one JSON object...}
#END
```

* results are printed as **one JSON object per line**
* the reply always ends with `#END` (that is what a host should wait for)
* a failure prints `#ERR <reason>` before `#END`
* anything else on the port is ordinary `ESP_LOG` output — ignore it

## Commands

| Command | What it does |
|---|---|
| `help` | print the command list |
| `status` | one line: `mode`, `adv`, `scan`, `detected`, `heap`, `boots`, `brownouts`, `reset_reason`, `adv_starts`, `tx_dbm`, `scan_pkts`, `storage`, profile count/active |
| `scan` | JSONL: the live capture table — `addr`, `name`, `rssi`, `itvl_ms`, `raw` / `rsp` / `mfg` **hex** |
| `profiles` | JSONL: the stored profiles (`id`, `active`, `raw`, `rsp`, address mode, ...) |
| `records` | JSONL: the persistent capture archive — `addr`, `name`, `raw`/`rsp` hex, `mfg_company`/`mfg_vendor`, `svc`/`svc_name`, `rssi`/`rssi_max`/`rssi_avg`, `packets`, `itvl_ms`, `dwell_ms`, `boot_seq`, `uptime_s` |
| `logs [since_seq]` | JSONL: ring log records |
| `mode sniff\|clone\|idle` | switch the radio (arbitrated by `radio.c`, same as the UI) |
| `use <id>` / `use -1` | activate profile `id` / broadcast nothing |
| `clone <addr>` | copy one row of the capture table into a profile and start advertising |
| `adv <hex>` | Build/update the `usb-raw` profile from a hex payload and start broadcasting — no capture table entry needed, so a payload captured earlier can be restored anywhere |
| `clear` | empty the capture table |
| `records clear` | empty the persistent archive |
| `pins` | JSONL: favourites (`id` per row, oldest first) |
| `pin <n>` | pin the n-th row of `records` (newest first — same order as the UI) |
| `unpin <n>` | drop favourite `n` |
| `pins clear` | drop all favourites |
| `records format` | format the partition — only while the archive is **off** (a fresh board, or one whose FAT was damaged). It never formats on its own. |
| `reboot` | restart |
| `factory yes` | factory reset, then restart |

`clone <addr>` and `POST /api/clone` are the same code (`main/clone_op.c`), so a clone made
over USB is byte-identical to one made from the browser: profile creation, address handling
(random pinned as static / public applied at boot), one archive write, radio switch.

The web page itself is the one thing the console cannot change: it is replaced over the SoftAP --
`python tools/ui_push.py --pass <AP password> main/www/index.html`, see
[Uploading a new UI](#uploading-a-new-ui).

## Host side

```bash
pip install pyserial

python tools/usb_cli.py -p COM3 status
python tools/usb_cli.py -p COM3 scan
python tools/usb_cli.py -p COM3 mode sniff      # 60 s capture window
python tools/usb_cli.py -p COM3 clone a4c1382a5122
```

`--raw` shows the raw lines (including log noise), `--timeout` widens the wait for slow
commands. Port names: `COM3` (Windows), `/dev/ttyACM0` (Linux), `/dev/cu.usbmodem*` (macOS).

A typical capture-then-clone session over USB only:

```
mode sniff        -> wait for the 60 s window, or just watch `scan`
scan              -> pick the row you want (raw hex included)
clone <addr>      -> becomes the active profile and starts advertising
status            -> verify mode=clone, adv=true
```

## Caveats

* **DTR/RTS**. USB-Serial/JTAG maps them onto BOOT/EN, so a tool that asserts them on open
  can reset the board (that reset is exactly what `esptool` wants). `tools/usb_cli.py` leaves
  them de-asserted; if your terminal program resets the board on attach, that is why.
* **One writer at a time**. `idf.py monitor`, `esptool` and this console all want the port.
  Closing the handle abruptly has reset the board in the past — exit `monitor` with `Ctrl+]`,
  close `usb_cli.py` normally.
* **Public-address clones reboot once** (see `radio_clone()`); the console reply is sent
  before the restart, so a `#END` followed by a boot banner is expected.
* **Log and reply share the port.** Nothing is lost, but a host that parses JSON must skip
  lines that do not start with `{`.
* `storage` archives on clone (one write) and whenever the radio leaves SNIFF (one batch for the
  whole table: window timeout, triple click, `mode idle`, web UI), never per packet. Favourites
  live in `/data/pin.bin`, untouched by scans. A double click starts a blind
  scan precisely so the result can be read out here after unplugging.

## Uploading a new UI

The SPA normally comes from the firmware image. To replace it without reflashing:

```bash
python tools/ui_push.py --pass <AP password> main/www/index.html   # upload
python tools/ui_push.py --pass <AP password> --status              # which copy is served
python tools/ui_push.py --pass <AP password> --revert              # back to the embedded one
```

`--host` defaults to `192.168.4.1`, so this one needs the SoftAP (or a route to it); `--pass`
accepts the AP password, which it trades for a token. The body must be gzip, 1..64 KB; the device
never inflates it. The tool prints the served copy before and after the upload (`embedded` →
`fs`, with the new size and ETag) and exits non-zero when the board refuses.

The embedded copy stays in the firmware as the fallback, so a broken upload -- or
`records format`, which wipes the whole FATFS partition -- cannot leave the page blank.
