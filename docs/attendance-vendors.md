# Attendance / access-control devices on air

How a 考勤机 / 门禁终端 identifies itself over BLE, and what this firmware can name.
Everything below comes from the public SIG assigned-numbers list plus open-source field
dumps; nothing here is vendor-confirmed documentation.

## Two mechanisms, and only two

A legacy advertisement is 31 bytes. A device either

1. carries **manufacturer specific data** (`len | 0xFF | company id | payload`), and the
   company id is what `blescan_company_name()` resolves, or
2. carries nothing but a **local name** (and maybe a 16-bit service UUID), which is what
   `blescan_name_vendor()` matches on.

A device with neither is anonymous: the capture table then only shows its address, RSSI and
raw bytes — which is still enough to clone it byte for byte, but not to say who made it.

## Company ids that do exist

From the SIG company identifier list (authoritative ids, names shortened for the UI):

| id | vendor | note |
|---|---|---|
| `0x0100` | TomTom International BV | **DingTalk's BLE check-in terminals advertise under this id** — see below |
| `0x0E25` | Hangzhou Hikvision (海康威视) | access control / face terminals |
| `0x0124` | HID Global | cards, readers |
| `0x013B` | Allegion | locks / access |
| `0x0C64` | dormakaba | locks / access |
| `0x0199` | SALTO Systems | electronic locks |
| `0x0B5D` | Fujian Newland (新大陆) | scanners, terminals |
| `0x01F1` | Zebra Technologies | handhelds |
| `0x0526` | Honeywell | handhelds |
| `0x0E60` | Ant Group (蚂蚁/支付宝) | Alipay hardware |
| `0x0EC5` | Alibaba | DingTalk / Alipay / Tmall Genie hardware |
| `0x011C` | Baidu | |
| `0x0025` | NXP | silicon, not the product |

## The DingTalk check-in terminal (钉钉蓝牙打卡机)

This is the only attendance device with public field dumps, and it is a good example of why
the company id matters more than the name:

```
02 01 16                          flags
17 ff 00 01 b5 00 02 01 59 5d ..  mfg data, company id 00 01 (LE) = 0x0100 = TomTom
03 03 3c fe                       16-bit service 0xFE3C (not in the published list)
```

* local name is usually `RTK_BT_…` (not always)
* in nRF Connect the row shows **TomTom** — that is the *company* name resolved from
  `0x0100`, not the device name. This is why community guides say "the signal marked
  TomTom is the DingTalk machine".
* the payload after the company id changes per site / per device
* `0xFE3C` is not in the published assigned-numbers list, but the firmware names it
  `DingTalk` (verified against a real terminal: company `0x0100`, payload `b5 00 02 …`,
  flags `02 01 16`, this service UUID, advertised every ~82 ms)

Sources: the `HIT-bangbang/Dingding-Assistant` README (ESP32 spoofing project, name +
raw dump instructions) and a blog post showing a real `nRF Connect` raw string.

Because the id is TomTom's, a genuine TomTom product would look the same. Treat the label
as a hint, not as proof.

## Vendors that advertise no company id

Searched the SIG list for the big attendance names and found **nothing**:

`ZKTeco (熵基 / 中控智慧)`, `Anviz`, `Suprema`, `Hanvon (汉王)`, `Dahua (大华)`,
`Deli (得力)`, `Sunmi (商米)`, `Telpo (天波)`

They are not unknown to the SIG — they simply do not put a company id in their
advertisements; their BLE is used for app-based provisioning (pair, then GATT), not for
beaconing. So the only handle on air is the local name, which is what `blescan_name_vendor()`
matches on:

| name contains | labeled |
|---|---|
| `RTK_BT` | DingTalk |
| `ZKTECO` | ZKTeco |
| `ANVIZ` | Anviz |
| `SUPREMA` | Suprema |
| `HIKVISION` | Hikvision |
| `DAHUA` | Dahua |
| `HANVON` | Hanvon |
| `SUNMI` | Sunmi |
| `TELPO` | Telpo |

## The built-in noise filter

A blind scan in a normal office fills every slot with phones, watches, earbuds and
Windows PCs within seconds. `scan.filter_noise` (default **on**) therefore drops a frame
*before* it takes a slot, when it is:

* Apple Continuity — any Apple manufacturer data except **iBeacon** (subtype `0x02`)
* Microsoft CDP / Swift Pair — company `0x0006`, payload starting `01 09 20 22`
* a name containing `buds / airpods / freebuds / edifier / qcy / soundcore / amazfit /
  mi band / smart band / smartwatch / watch`
* a phone vendor (`Xiaomi, OPPO, vivo, Honor, Huawei, OnePlus, Samsung, Google`) **without**
  a 16-bit service UUID — that keeps MiBeacon sensors (`0xFE95`) and friends

`/api/status` reports `scan_filtered` (how many frames were dropped) next to
`scan_pkts`, and the USB console has `filter on|off`.

## How to actually identify one

```bash
python tools/usb_cli.py -p COM7 mode sniff     # 60 s window
python tools/usb_cli.py -p COM7 scan           # or watch the UI's Signals tab
```

Then read, per row:

* `mfg_company` + `mfg_vendor` — the SIG id and name (when the device sends one)
* `svc` / `svc_name` — the 16-bit service UUID, for devices that only use service data
* `name_vendor` — the guess from the local name, only present when the name matched
* `raw` — the 31 bytes, which is what the clone replays verbatim

If a row shows none of the three, capture it, note the address, and add the name prefix (or
the company id, if it turns out to send one) to `blescan_name_vendor()` /
`blescan_company_name()` in `main/beacon_scan.c`. Two lines, no other changes.

> These labels are for recognising devices on a bench, not for any security decision. Both
> the company id and the local name are trivially spoofable — that is exactly what this
> project does when it clones an advertisement.
