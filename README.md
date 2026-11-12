# ESP32-C6 BLE Beacon Cloner

Scan nearby BLE advertisements, then **replicate them byte-for-byte**.

Built for a single **ESP32-C6** running ESP-IDF. Capturing and broadcasting are
**mutually exclusive** modes — that removes self-capture feedback loops and radio
time-sharing contention on a single-radio SoC.

```
 ┌──────────┐   pick one    ┌───────────┐
 │  SNIFF   │ ───────────▶  │   CLONE   │
 │ scan only│               │  adv only │
 └──────────┘ ◀───────────  └───────────┘
      ▲   raw 31-byte payload captured        identical bytes replayed
      └────────────── phone / browser ───────────────┘
```

> **Legal notice.** This tool can replay arbitrary BLE advertisements, i.e. it can
> impersonate other beacons. Use it **only on devices you own or are explicitly
> authorized to test**. Replicating a third party's beacon identifiers may be
> illegal in your jurisdiction and may disrupt deployed positioning systems.
> "iBeacon" is a trademark of Apple Inc.; this project is only *iBeacon compatible*.

---

## Features

| | |
|---|---|
| **Raw replication** | Captures the full 31-byte advertising payload and replays it with `ble_gap_adv_set_data()` — works for iBeacon, Eddystone-UID/URL, AltBeacon, plain named BLE devices and private formats |
| **Metadata replication** | Re-estimates the advertising interval (EMA over inter-arrival gaps), preserves scannability, optionally clones a static-random address |
| **Exclusive modes** | `sniff` / `clone` / `idle` arbitrated by one module (`radio.c`) with a mutex — no self-capture, no radio contention |
| **3 profile slots** | Stored atomically in NVS; when full the least-recently-used non-active slot is overwritten |
| **Persistent capture archive** | Last 32 captures in a FATFS+wear-levelling ring file, survives power loss, written once per clone |
| **Phone-only operation** | Single-page app is gzipped and embedded in the firmware (10 KB) — no internet, no CDN, no app install |
| **Serial-free diagnostics** | `/api/status` exposes `reset_reason`, brownout counter, TX power, scan packet count, heap |
| **Board presets** | Seeed XIAO ESP32-C6 (default), Espressif DevKitC-1, Waveshare C6-Zero — build-time switch |
| **LED + one button** | Six LED states distinguishable on a *single-colour* LED; single click / double click / long press |

**Not implemented:** OTA update (`/api/ota`), extended advertising (>31 bytes), active scanning
(scan response), RPA addresses. See [Limitations](#limitations).

---

## Hardware

| Board | Notes |
|---|---|
| **Seeed XIAO ESP32-C6** (default, `APP_BOARD=2`) | Single-colour user LED on **GPIO15**, BOOT button **GPIO9**, native USB-JTAG, battery pads + u.FL, 4 MB flash |
| Espressif ESP32-C6-DevKitC-1 (`APP_BOARD=1`) | WS2812 RGB on **GPIO8**, BOOT **GPIO9**, two USB ports, documented schematic |
| Waveshare ESP32-C6-Zero (`APP_BOARD=3`) | Verify silkscreen before trusting the defaults |

Only **two GPIOs** are used (LED + button), so any C6 board works after editing `main/board.h`.
Native USB is **GPIO12/GPIO13** on the C6 (unlike the C3, which uses 18/19).

**Power:** ~110–130 mA average with the SoftAP up, 350–420 mA peaks.
A CR2032 coin cell cannot do this (10–40 Ω internal resistance collapses under the
current spike). Use USB, a power bank, 3×AA (4.5 V) or a CR123A.

---

## Quick start

```bash
# ESP-IDF >= v5.2 required (v5.5.6 verified)
. $IDF_PATH/export.sh

idf.py set-target esp32c6
idf.py -DAPP_BOARD=2 build          # 2 = XIAO C6 (default if omitted)
idf.py -p /dev/ttyACM0 flash monitor
```

1. The board brings up SoftAP `BeaconGW-XXXX`, password `beaconXXXX` (last two MAC bytes,
   also printed on the serial console).
2. Join it, open <http://192.168.4.1/>.
3. Log in with the AP password → you get an API token.
4. **Capture** tab → wait a few seconds, find the target, tap **Copy**.
   The device stops scanning, creates a RAW profile and starts broadcasting it.
5. Verify with nRF Connect: the advertisement should be byte-identical.

### Verifying without a serial console

`GET /api/status` is designed to answer "is my power supply good enough?":

```jsonc
{
  "board": "XIAO ESP32-C6",
  "reset_reason": "brownout",     // power_on | software | panic | int_wdt | task_wdt | brownout
  "boots": 17, "brownouts": 3,    // persisted in NVS
  "mode": "sniff", "led": "sniff",
  "tx_dbm": 9, "adv_starts": 4,
  "scan_pkts": 1832, "scan_running_ms": 41200,
  "storage": "12/32"
}
```

| Observation | Meaning |
|---|---|
| `brownouts` increasing | Power supply is not good enough — not a firmware bug |
| `mode=clone` but `adv_starts` frozen | Radio failed to start, check `/api/log` for `adv_start rc=` |
| `mode=sniff` but `scan_pkts` stays 0 | Antenna / distance / interval; raise `win_ms` |
| `heap` monotonically falling | Memory leak |

---

## How it works

### Capture

Passive scan (`BLE_GAP_DISC_MODE`, duty cycle capped at 50% to keep the SoftAP alive),
`filter_duplicates = 0` so the application keeps its own counters. Per device we store:

- `raw[31]` — the complete AD structure, verbatim
- `adv_type` — `ADV_IND` / `ADV_SCAN_IND` / `ADV_NONCONN_IND` …
- `itvl_ms` — EMA of inter-arrival gaps (clamped to 20 ms … 10 s)
- RSSI last/max/IIR-average, packet count, first/last seen

Detection table is 16 entries, LRU evicted, **RAM only** (updated per packet — must
never touch flash).

### Clone

`radio_clone()` stops scanning first, then:

```c
ble_gap_adv_set_data(p->raw, p->raw_len);   /* verbatim, no re-encoding */
```

and starts advertising with the estimated interval, the source's scannability and
(if the source address was random) a cloned static-random address.

Connectable sources (`ADV_IND`) are deliberately replayed as **non-connectable**:
we do not replicate the GATT server, so accepting a connection would only fail.
Downstream beacon systems only read advertisements.

### Address cloning — what is physically possible

| Source address type | Cloneable | Why |
|---|---|---|
| Static random | ✅ identical | `ble_hs_id_set_rnd()` accepts it |
| Public | ⚠️ bytes only | You cannot forge the public type; the same 6 bytes are sent as a *random* address, and the controller forces the top two bits to `11` — up to 2 bits may differ |
| RPA (resolvable private) | ❌ pointless | Rotates every ~15 min at the source |

---

## LED and button

Six states on a **single-colour** LED (the XIAO has no RGB):

| Pattern | Meaning |
|---|---|
| Double blink (2×100 ms / 1.6 s) | **Sniffing** |
| 1 Hz slow blink | **Cloning / advertising** |
| Triple fast blink | Cloning **and** a client is on the web UI |
| One blink every 3 s | Idle (profiles exist, radio off) |
| Solid | No profile at all |
| 6 rapid blinks | Radio error |

RGB boards (DevKitC-1) additionally use colour: cyan = sniff, blue = clone, green = clone+client, amber = idle.

| Button gesture (BOOT / GPIO9) | Action |
|---|---|
| Single click | Sniff ⇄ Clone |
| Double click (<450 ms) | Stop radio → idle |
| Long press 5 s | Factory reset + reboot |

---

## Storage layout (4 MB flash)

| Partition | Offset | Size | Purpose | Used |
|---|---|---|---|---|
| nvs | 0x9000 | 24 KB | 3 profiles + token + counters | <2 KB |
| otadata / phy_init | 0xF000 | 8 / 4 KB | OTA state, RF calibration | — |
| ota_0 / ota_1 | 0x20000 / 0x1A0000 | 1536 KB ×2 | firmware + rollback slot | 1144 KB |
| storage (FAT) | 0x320000 | 832 KB | `/data/cap.bin` capture archive | 2.2 KB |

| Data | Where | Survives power loss | Write rate |
|---|---|---|---|
| Profiles (≤3, incl. raw bytes) | NVS single blob, atomic | ✅ | on activate/edit |
| Token, boot & brownout counters | NVS | ✅ | very low |
| Current capture table (16) | **RAM** | ❌ | every packet |
| Event log (128) | RAM ring | ❌ | per event |
| Capture archive (32) | **FATFS + wear levelling**, 70 B ring records | ✅ | one write per clone |
| Web SPA | app rodata, gzipped at build time (10 KB) | firmware | — |

NVS wear: a ~700 B blob is ~22 entries; 24 KB = 6 pages × 126 entries with page-level
wear levelling → ~3.3 M effective writes ≈ 45 years at 200 activations/day.
The rule that actually matters: **never put per-packet data in flash.**

The archive is FATFS rather than LittleFS on purpose — LittleFS is not part of
ESP-IDF core and would add an external managed component for no benefit at this
write rate. FAT + `wear_levelling` is in-tree and equally power-loss tolerant here.

---

## REST API

All endpoints except `POST /api/session` require `Authorization: Bearer <token>`.

| Method | Path | Description |
|---|---|---|
| POST | `/api/session` | `{"password":"<AP password>"}` → `{"token":...}` |
| GET | `/api/status` | board, uptime, heap, mode, reset reason, counters, stats |
| GET | `/api/scan?limit=` | current capture table (includes `raw` hex, `itvl_ms`, `adv_type`) |
| POST | `/api/scan` | `{"enabled":bool,"itvl_ms":n,"win_ms":n}` |
| DELETE | `/api/scan` | clear capture table |
| POST | `/api/clone` | `{"addr":"<12 hex>"}` → stop scan, create profile, broadcast |
| POST | `/api/mode` | `{"mode":"sniff"\|"clone"\|"idle"}` |
| GET/DELETE | `/api/history` | persistent capture archive / clear |
| GET | `/api/profiles` | profiles + active + scan + ap + rotation |
| POST | `/api/profiles` | create |
| PUT/DELETE | `/api/profiles/<id>` | edit / delete |
| POST | `/api/active` | `{"id":0}` activate, `{"id":-1}` stop |
| POST | `/api/rotation` | `{"enabled":bool,"seconds":n}` |
| GET/DELETE | `/api/log` | incremental event log (`since`, `limit`) |
| GET/POST | `/api/export` `/api/import` | whole config JSON |
| POST | `/api/ap` | change SSID/password |
| POST | `/api/reboot` `/api/factory` | reboot / factory reset |

```bash
T=<token>
curl -H "Authorization: Bearer $T" -X POST http://192.168.4.1/api/mode -d '{"mode":"sniff"}'
curl -H "Authorization: Bearer $T" "http://192.168.4.1/api/scan?limit=10"
curl -H "Authorization: Bearer $T" -X POST http://192.168.4.1/api/clone -d '{"addr":"aabbccddeeff"}'
# → {"ok":true,"id":0,"mode":"clone","adv_itvl_ms":100,"replaced":false}
```

---

## Build and verification log

Verified locally with **ESP-IDF v5.5.6** / riscv32-esp-elf 14.2.0:

| Item | Result |
|---|---|
| Build | ✅ clean, **zero warnings in project sources** |
| Image | 1144 KB in a 1536 KB slot (392 KB headroom) |
| Static RAM | 186,786 B / 452,112 B = **41.3 %** → ~265 KB dynamic heap |
| Frontend | gzip-embedded, 10,216 B (26.6 KB raw) |
| Presets | both `APP_BOARD=1` and `=2` build |

Three issues were found by actually compiling (do not reintroduce them):

1. **`ble_svc_gap_*` is unlinkable** when `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n` —
   the GAP service is only compiled into `libbt.a` for peripheral builds.
   This project is broadcaster + observer only, so no GAP/GATT init is allowed.
2. **`scan_start` collides with `libnet80211.a(ieee80211_scan.o)`** when Wi-Fi is
   enabled. All scan-side symbols are therefore namespaced `blescan_*`.
3. Invalid `sdkconfig.defaults` entries: `CONFIG_LOG_DEFAULT_LEVEL_INFO` needs `=y`,
   `CONFIG_ESP_COEX_SW_COEXIST_PREFERENCE_BALANCE` does not exist in v5.5, and
   `CONFIG_BT_CTRL_MODEM_SLEEP` exists **only on ESP32-C3** (use `CONFIG_PM_ENABLE`
   on C6 for low-power advertising).

---

## Limitations

1. **Extended advertising (>31 bytes) cannot be replicated.** Legacy advertising data
   is capped at 31 bytes.
2. **Passive scan only** — device names carried in the scan response are invisible.
   Adding active scanning is a small change (it costs airtime).
3. **Public addresses cannot be fully cloned** (see the table above).
4. **No OTA** in this version; update over USB.
5. Compile-verified but **not yet run on hardware** — RF behaviour, LED polarity and
   supply headroom still need a bench run.

### Coexistence note (why modes are exclusive)

Espressif's official RF coexistence table rates *SoftAP connected* × *BLE scan/advertising*
as **`C1` — "supported but the performance is unstable"** for ESP32-C3, C6 **and** C5 alike
(the tables are cell-for-cell identical). Making capture and broadcast mutually exclusive
sidesteps that rating entirely, and also removes the risk of capturing your own
replayed packets.

---

## Repository layout

```
main/
├── board.h            board presets (LED/button GPIO)
├── main.c             boot, mode LED mapping, button callbacks
├── radio.[ch]         RF mode arbiter: sniff / clone / idle (mutex)
├── beacon_scan.[ch]   passive observer, raw payload capture, aggregation
├── beacon_adv.[ch]    NimBLE broadcaster, iBeacon + raw replay
├── app_cfg.[ch]       NVS config blob, JSON validation, NVS counters
├── storage.[ch]       FATFS+WL capture archive (ring of 70 B records)
├── app_log.[ch]       RAM ring event log with sequence numbers
├── status_led.[ch]    LED pattern engine (mono + WS2812)
├── btn.[ch]           single click / double click / long press
├── net_ap.[ch]        SoftAP + station accounting
├── web_server.[ch]    REST API + embedded SPA + bearer auth
└── www/index.html     single-page UI (gzipped at build time)
```

## License

MIT — see [LICENSE](LICENSE).
