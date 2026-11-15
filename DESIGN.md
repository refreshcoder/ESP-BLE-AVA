# Design Notes — ESP32-C6 BLE Beacon Cloner

Status: **v1.3**, compile-verified on ESP-IDF v5.5.6, not yet run on hardware.

This document records *why* the implementation looks the way it does: platform limits,
the frame layout we replicate, the mode arbitration model, storage decisions, and the
measured/estimated budgets. It deliberately keeps the analysis that led to decisions, so
future changes can be checked against it.

---

## 1. Scope

The product was narrowed twice, and both reductions are load-bearing:

1. From "7×24 beacon gateway" to **"scan a signal, then replicate it"**.
2. **Capture and broadcast are mutually exclusive** — never simultaneous.

Everything else follows from those two sentences. What they eliminate:

| Original problem | Now |
|---|---|
| Scan + advertise + SoftAP all sharing one radio, officially rated `C1` ("unstable") | Gone. At most **one** BLE role is active at a time |
| Scanning your own replay → device clones itself → infinite clone chain | Structurally impossible |
| Capping scan duty cycle at 30 % to protect the advertiser | Raised to **50 %**; only the SoftAP competes |
| Rotation, 24×7 soak, log persistence, per-unit RSSI calibration | Optional or dropped |

---

## 2. Platform constraints

ESP32-C6: single RISC-V core @160 MHz (+ LP core), 512 KB SRAM, 4 MB flash,
Wi-Fi 6 (2.4 GHz) + BLE 5.3 + 802.15.4 — **one RF path**, time-division multiplexed.

**ESP32-C3 variant (also supported):** single RISC-V core @160 MHz, 400 KB SRAM,
Wi-Fi 4 + BLE 5.0, no 802.15.4, native USB on GPIO18/19. The code is identical; the
budget shifts as follows (both measured on v5.5.6):

| | ESP32-C6 | ESP32-C3 |
|---|---|---|
| Image | 1144 KB | 973 KB |
| Static RAM | 186,786 B (41.3 % of 452,112) | 151,520 B (47.2 % of 321,296) |
| Heap headroom at startup | ~265 KB | ~166 KB |
| Coexistence rating (official table) | `C1` for SoftAP×BLE | `C1` — identical |

The tighter C3 heap is the only real constraint: fine for this firmware, but a reason to
prefer the C6 if the UI or the archive grows substantially.

Decisions taken because of this:

- **802.15.4 disabled** (`# CONFIG_IEEE802154_ENABLED is not set`) — it is a third
  consumer of the same radio with no role here.
- **NimBLE, not Bluedroid**, with peripheral/central roles compiled out: only
  broadcaster + observer. This is also why the GAP service is not linked (§12).
- **No TLS on the management UI.** A TLS handshake costs ~20 KB RAM and ~1.5 s on this
  class of part; the SoftAP is a closed network and the API is bearer-token protected.
- **Content is embedded, not served from flash FS**: the SPA is gzipped at build time
  and linked into rodata (15 KB), so the device works with no internet and never
  touches the filesystem to render its own UI.

### Typical current draw (estimates, not measured)

| Mode | Average | Peak |
|---|---|---|
| SoftAP + advertising 100 ms | 110–130 mA | 350–420 mA |
| SoftAP only | 85–100 mA | ~250 mA |
| Sniffing, 50 % duty | 55–75 mA | ~250 mA |
| Advertising only, 100 ms | 25–40 mA | ~150 mA |

Consequence: the 350–420 mA peak must be served by a real supply with bulk capacitance.
A CR2032 (10–40 Ω internal resistance) collapses the rail and triggers brownout reset.

---

## 3. What exactly gets replicated

### 3.1 Legacy advertising data (the part we copy)

iBeacon is the best-known case; it is a 30-byte legacy advertisement:

```
off   bytes                    meaning
0x00  02 01 06                 Flags: LE General Discoverable, BR/EDR not supported
0x03  1A                       length of the next AD structure (26)
0x04  FF                       AD type: manufacturer specific data
0x05  4C 00                    company ID 0x004C (Apple, little endian)
0x07  02                       subtype: beacon
0x08  15                       subtype length: 21
0x09  .. (16 bytes)            proximity UUID
0x19  XX XX                    major (big endian)
0x1B  XX XX                    minor (big endian)
0x1D  XX                       measured power / RSSI at 1 m (int8, signed)
```

Notes:

- The last byte is **not** the radio TX power; it is the calibrated RSSI at 1 metre
  (typically −59…−65). The radio power is configured separately.
- Valid only with company ID 0x004C, subtype 0x02 and length 0x15 — phones check all three.
- `ADV_NONCONN_IND` is the honest choice for a beacon (lowest power, cannot be connected).

### 3.2 Why we do not rebuild the frame

Structured rebuild (`ble_gap_adv_set_fields`) can only reproduce iBeacon-shaped data.
To replicate *arbitrary* advertisements we keep the raw bytes and replay them:

```c
ble_gap_adv_set_data(p->raw, p->raw_len);   /* verbatim */
```

This makes iBeacon, Eddystone-UID/URL, AltBeacon, private formats and plain named
devices all work through the same code path. iBeacon becomes a special case.

### 3.3 Metadata that is also replicated

| Field | Source | Notes |
|---|---|---|
| `adv_itvl_ms` | EMA of inter-arrival gaps | Falls back to 100 ms if only one packet was seen |
| `scannable` | source `event_type` (ADV_IND/ADV_SCAN_IND) or a captured SCAN_RSP | connectable sources are replayed as `ADV_SCAN_IND`, others as `ADV_NONCONN_IND` |
| `rsp[31]` | the source's scan response, verbatim | replayed with `ble_gap_adv_rsp_set_data()`; requires `scannable` |
| static-random address | source address when its type is random | forced top two bits `11` |

Deliberate omission: when the source is connectable we replay **non-connectable**.
We do not replicate the GATT server, so accepting connections would only fail; every
downstream beacon system reads advertisements only.

### 3.4 Address cloning limits

| Source type | Result | Reason |
|---|---|---|
| Static random | identical | `ble_hs_id_set_rnd()` sets it verbatim |
| Public | identical, after one reboot | `esp_iface_mac_addr_set(ESP_MAC_BT, …)` overrides the controller's public address **in RAM**, but only before the stack is up — so it is applied once per boot and changing it reboots the device (§3.5) |
| RPA | pointless | rotates every ~15 min at the source |

Practically irrelevant for beacon use (systems key on UUID/major/minor or manufacturer
data), fatal for MAC allow-lists.

### 3.5 Cloning a public address (boot-time override)

The public address is not a free-form field in the advertising PDU — it comes from the controller's
own identity, so `ble_gap_adv_start()` has no parameter for it. It *can* be overridden in RAM
from the IDF side, without burning eFuse:

```c
esp_iface_mac_addr_set(ESP_MAC_BT, addr);   /* before any interface is initialised */
```

The documentation is explicit that this must happen **before any network interface is initialised
or `esp_read_mac()` is called**, because the interface MAC is derived and cached on first read. So:

| | |
|---|---|
| Where it is applied | `app_main()`, before `net_ap_start()` / `ble` stack init, for the then **active** profile only |
| Scope | one address per boot; not per profile, not switchable at runtime |
| Switching profiles | `radio_clone()` compares the active profile against the address this boot advertises with (`cfg_addr_change_pending()`) and schedules a reboot 1.2 s later |
| Wi-Fi side | untouched — `ESP_MAC_BT` only, so the SoftAP SSID/password derivation stays valid |
| Rotation | paused while any profile clones a public address (it would reboot on every step) |
| Failure mode | `esp_iface_mac_addr_set()` returns an error for a multicast address; the log line `ble public addr <- …` shows the result |

Because the address is set before the BLE stack reads it, the advertisement carries
`TxAdd = 0` — on air it is indistinguishable from the original device's public address, unlike the
static-random fallback (which is forced to top bits `11`).

Legally this is impersonation of an IEEE-assigned identity, not just payload replay: keep it to
owned or explicitly authorised devices (see §14).

---

### 3.6 Scan response (the second 31 bytes)

A legacy advertiser may answer `SCAN_REQ` with up to 31 bytes of `SCAN_RSP`. Once extended
advertising is out of scope, that is the *only* other payload on the link, and some devices put
their data there (complete names, more service data) or split it between `ADV` and `SCAN_RSP`.

Consequences for the design:

- The observer must scan **actively** (`params.passive = 0`), otherwise it never sees a response.
  It arrives as a second `BLE_GAP_EVENT_DISC` for the same address with
  `event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP (0x04)`.
- A scan response is **not** an advertisement: it must not overwrite `raw`, the interval
  estimate or the packet counter — it goes into `rsp[31]` and returns immediately.
- Replay needs `ble_gap_adv_rsp_set_data()` **and** a scannable advertisement
  (`ADV_SCAN_IND`). A `SCAN_RSP` on a non-connectable non-scannable advertisement is
  unreachable on air, so a captured response forces `scannable = true`.
- The response is (re)set on every `apply`, including the empty case, so a previous profile's
  scan response cannot leak into the next one.
- Both buffers are ≤ 31 B, so a profile grows by 32 B (payload + length byte); 3 slots stay
  far below the NVS blob limit.
- Cost: an active observer transmits one `SCAN_REQ` per advertisement it hears. Modes are
  exclusive, so this never competes with our own advertising, and the official SoftAP × BLE-scan
  coexistence rating (`C1`) is unchanged.

---

## 4. Architecture

```
app_main
 ├── cfg_init      (NVS)
 ├── storage_init  (FATFS + wear levelling, optional)
 ├── scan_init
 ├── radio_init    ← single owner of RF mode
 ├── status_led / btn
 ├── net_ap_start  (SoftAP first; coexistence params latch at BLE init)
 ├── web_start     (httpd)
 └── beacon_stack_init → NimBLE host task → sync_cb → radio_on_ble_ready()
```

**Rule: nobody outside `radio.c` may call `blescan_start()` or `beacon_adv_apply()`.**
`radio.c` holds a mutex and always performs "stop the old role → start the new one".

| Task | Prio | Stack | Notes |
|---|---|---|---|
| nimble_host | 21 | 4096 | no I/O, no cJSON in callbacks |
| httpd | 5 | 7168 | cJSON serialisation lives here |
| led | 3 | 3072 | 50 ms tick |
| btn | 4 | 2560 | 20 ms sampling |
| esp_timer tick | — | — | 1 s: rotation + LED state |

Scan callbacks do **parse + table insert only**. Any logging is throttled to one line
per device per 5 minutes.

---

## 5. Modes

```
RMODE_IDLE ──┬─▶ RMODE_SNIFF   (scan only, records raw[31])
             └─▶ RMODE_CLONE   (advertise only, verbatim bytes)
```

Transitions: UI buttons, `POST /api/mode`, or the BOOT button.

**Scanning is manual-only.** It never starts by itself: not at boot, and not from the BOOT
button. The button is a broadcast on/off toggle, and a boot restores either "broadcasting" or
"parked with the radio off" (`cfg.last_mode`); sniffing is started from the management page and
stops when you leave it. Rationale: the previous "capture first" default meant a reboot could
silently put a beacon that must keep transmitting into scan mode — which is exactly what happens
after an address switch, since that needs a reboot (§3.5).

Scanning still has its own persisted flag (`scan.enabled`), but asking for sniff explicitly now
overrides it, so the button can never appear dead.

A manual sniff **stops on its own after 30 s** (`radio_sniff_timed_out()` checked by the 1 s
tick, then `radio_stop()`), so a forgotten browser tab cannot leave the radio scanning forever;
tapping Sniff restarts the window. The window length is one `#define` in `radio.c`, not a setting:
anymore knobs and the "why did it stop" question gets a different answer every time.

Off-board retention: every UI poll is mirrored into the browser's `localStorage`, so captures
survive board reboots and the 16-entry RAM table. The board keeps only the ring archive
(one record per clone) — see §8.

Scan duty cycle is capped at **50 %** of the interval (`blescan_start()`); above that the
SoftAP stops answering. Higher duty improves capture probability, so the cap is exposed
as `scan.win_ms` / `scan.itvl_ms`.

Aggregation key is `addr_type + addr` (not UUID), because several devices may share a UUID.
Controller duplicate filtering is **off** — the application keeps its own counters.

---

## 6. Coexistence: the honest version of the table

Espressif publishes a per-chip RF coexistence matrix. For **Wi-Fi SoftAP × BLE** the
ESP32-C3, ESP32-C6 and ESP32-C5 tables are **cell-for-cell identical**:

| Wi-Fi state | BLE scan | BLE adv | BLE connected |
|---|---|---|---|
| STA scan / connecting / connected | Y | Y | Y |
| **SoftAP TX beacon** | Y | Y | Y |
| **SoftAP connecting / connected** | **C1** | **C1** | **C1** |

`Y` = supported and stable, `C1` = **supported but performance unstable**, `X` = not supported.

Two consequences that drove the design:

1. The "phone connected to the management page while the beacon advertises" scenario is
   officially rated unstable **on every chip**, so no chip upgrade fixes it — only
   avoiding the overlap does.
2. 5 GHz Wi-Fi does not remove the problem. The same document states:
   *"This shared RF path is used whether Wi-Fi operates on 2.4 GHz or 5 GHz."*
   The benefit of 5 GHz is spectral separation, not parallel radios.

Also worth recording: **no Espressif part has two independent 2.4 GHz radios.** Guaranteed
simultaneous scan + advertise + AP requires two chips (e.g. C6 + ESP32-H2 as a dedicated
BLE co-processor).

---

## 7. Data model

### 7.1 Profile

```jsonc
{
  "name": "store-A-entry",
  "enabled": true,
  "kind": 1,                    // 0 = structured iBeacon, 1 = raw replay
  "raw": "0201061aff4c0002...", // 2..31 bytes, required when kind = 1
  "uuid": "e2c56db5...",        // kind 0 only (informational for kind 1)
  "major": 100, "minor": 1,
  "ref_rssi": -59,              // iBeacon byte 30
  "radio_dbm": 9,
  "adv_itvl_ms": 100,           // 20..10240
  "scannable": false,
  "use_rand_addr": true, "rand_addr": "c0ffee000001",
  "src": "aabbccddeeff",        // provenance: which device was copied
  "src_adv_type": 0,
  "use_seq": 12                 // for least-recently-used eviction
}
```

### 7.2 Config blob (NVS key `beacon/cfg`, single atomic write, ~400 B with 3 profiles)

```jsonc
{ "ver": 1, "count": 3, "active": 0, "seq_gen": 12,
  "rotation": {"enabled": false, "seconds": 30},
  "scan": {"enabled": true, "itvl_ms": 300, "win_ms": 60},
  "ap": {"ssid": "BeaconGW-3F2A", "pass": "beacon3f2a"},
  "profiles": [ /* ... */ ] }
```

One blob rather than one key per profile: fewer writes, single consistent snapshot,
still far below the ~4 KB NVS blob limit. On parse failure we fall back to defaults
and log the reason instead of bricking the device.

Three slots is a product decision; when full, `cfg_pick_slot()` returns the profile with
the smallest `use_seq` that is not currently active (never evicting what is on air).

### 7.3 Detection (RAM)

```c
typedef struct {
    uint8_t  addr[6], addr_type, adv_type, raw_len, raw[31];
    bool     is_ibeacon;
    uint8_t  uuid[16]; uint16_t major, minor; int8_t ref_rssi;
    int8_t   rssi_last, rssi_max, rssi_avg;   // avg = IIR (avg*7 + new)/8
    uint32_t count, first_ms, last_ms;
    uint16_t itvl_ms;                          // EMA of gaps
} detection_t;                                  // ~95 B × 16 slots
```

### 7.4 Capture archive (flash, ring)

`/data/cap.bin` = 12-byte header (`magic, count, head`) + **32 × 70 B fixed records**
(`magic, boot_seq, uptime_s, addr, adv_type, raw[31], itvl_ms, rssi, …`).

Fixed-size ring instead of a growing log: the write is always one record plus a
12-byte header update, so there is no file growth, no garbage collection and no
compaction. Worst case on power loss: the last capture is lost.

There is no wall clock on the board, so time is expressed as
**`boot_seq` (which boot) + `uptime_s` (how long after boot)**.

---

## 8. Storage decisions

### 8.1 Flash budget (4 MB, plan B)

| Partition | Offset | Size | Purpose | Used |
|---|---|---|---|---|
| nvs | 0x9000 | 24 KB | config, token, counters | <2 KB |
| otadata / phy_init | 0xF000 | 8 / 4 KB | OTA state, RF cal | — |
| ota_0 | 0x20000 | 1536 KB | firmware | 1144 KB |
| ota_1 | 0x1A0000 | 1536 KB | rollback slot | — |
| storage | 0x320000 | 832 KB | FAT archive | 2.2 KB |

Chosen over two alternatives: a larger app slot (1792 KB) leaves the FS at 320 KB, and
dropping OTA entirely frees the most space but removes field updates. 392 KB of app
headroom is enough for OTA support and a bigger UI.

### 8.2 What must never go to flash

Per-packet data. The capture table updates tens of times per second; writing that to NVS
would exhaust the 24 KB partition in days and to FATFS would thrash the medium.
Archive writes happen **once per clone** (human-driven, a few per day).

### 8.3 Why FATFS and not LittleFS

LittleFS is not in ESP-IDF core; it ships as a managed component and would add an
external dependency and a network-dependent build for no benefit at this write rate.
`fatfs` + `wear_levelling` are in-tree, power-loss tolerant enough for an append-only
ring, and cost ~4 KB of RAM at mount time.

### 8.4 NVS wear arithmetic

Blob ≈ 700 B ≈ 22 entries of 32 B. Partition = 6 pages × 126 entries. A write consumes
~0.18 page; with page-level wear levelling the effective endurance is
≈ 100 k × 33 ≈ **3.3 M writes** ≈ 45 years at 200 activations/day. No optimisation needed.

---

## 9. Management UI

The SPA follows the product's single axis — **capture → pick → clone**:

| Tab | Content | Key controls |
|---|---|---|
| **Status** | Large current-mode label, LED pattern, brownout banner, last 3 captures | Three 58 px buttons: sniff / clone / idle |
| **Capture** | Address, type, UUID or name, major/minor, RSSI (last/avg), estimated interval, age, packet count | Freeze, filter chips (all / iBeacon / named / strongest), per-row **Copy**, expandable `hex` of the raw payload |
| **Profiles** | 3 cards: kind badge, raw hex prefix, interval, TX power, source address, on-air highlight | Activate / stop / advanced edit / delete |
| **Log** | Incremental event log, coloured by type | Refresh / clear |
| **Settings** | Scan window/interval, AP credentials, rotation, import/export, danger zone | — |

Design decisions worth keeping:

1. **Primary action on the first screen.** Capture and clone are not in a submenu;
   the buttons are thumb-sized.
2. **Raw bytes are always inspectable.** Without a hex view the user cannot tell whether
   the replication is correct.
3. **The capture table is labelled as volatile** ("lives in RAM, lost on reboot") while the
   archive below it is the persistent one — so nobody mistakes one for the other.
4. **No silent eviction.** When the 3rd slot is recycled, the API reports `replaced: true`
   and the UI says so.
5. **Human error messages**: `409 → "all profile slots are used"`, `404 → "no longer in the capture table, scan again"`, `401 → re-login`.
6. Dark, high-contrast theme (warehouses, outdoors), no web fonts, no CDN, works offline.
7. 2.5 s polling rather than WebSockets: fewer moving parts, tables are ≤16 rows.
8. **Two languages, one file.** English is the source and 简体中文 sits beside it in one
   inline `I18N` table keyed by `data-i18n` attributes; `localStorage.bgw_lang` remembers
   the choice and `navigator.language` seeds it. No JSON fetch, no second HTTP request,
   no extra flash partition — a language the device cannot serve never appears.

---

## 10. RSSI → distance (informational)

```
d = 10 ^ ((ref_rssi − rssi) / (10 · n))     n ≈ 2.0 open air, 2.5–3.5 indoors
```

`ref_rssi` must be calibrated per unit: place the device 1 m from a phone, average the
RSSI for 30 s, write that value back. Board-to-board differences of ±3 dB are normal, so
never copy a calibration value between units — especially not from a board with a small
PCB antenna to one with an external antenna.

---

## 11. What is still unverified

Compile-verified only. Remaining bench work:

| Item | How to check |
|---|---|
| LED GPIO/polarity per board | Does the pattern match sniff/clone/idle? |
| Button gestures | single / double / long press all fire |
| Actual capture + replay | nRF Connect side-by-side byte comparison |
| Supply headroom | watch `brownouts` in `/api/status` under SoftAP + advertising |
| Scan duty vs SoftAP responsiveness | raise `win_ms` until the page stalls |

---

## 12. Defects found by compiling (do not reintroduce)

1. `ble_svc_gap_*` is not linked when `CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n`; the GAP
   service is compiled into `libbt.a` only for peripheral builds. Broadcaster + observer
   must not initialise GAP/GATT.
2. `scan_start` collides with `libnet80211.a(ieee80211_scan.o)` once Wi-Fi is enabled.
   Scan-side symbols are namespaced `blescan_*`.
3. `CONFIG_LOG_DEFAULT_LEVEL_INFO` needs `=y`; `CONFIG_ESP_COEX_SW_COEXIST_PREFERENCE_BALANCE`
   does not exist in v5.5; `CONFIG_BT_CTRL_MODEM_SLEEP` exists **only on ESP32-C3**
   (use `CONFIG_PM_ENABLE` + tickless idle on C6, and remember it is incompatible with
   keeping the SoftAP up).

---

## 13. Roadmap / known limitations

- **Extended advertising (>31 B) is not replicated.** Legacy limit.
- **Scan responses are replayed but not parsed.** Active scanning captures `rsp[31]` and the
  clone answers `SCAN_REQ` with it, but a device whose *name* lives only in the SCAN_RSP still
  shows up as unnamed in the UI (only the advertisement is parsed for name/iBeacon fields).
  The capture archive does not store the scan response either.
- **No OTA** yet: `esp_ota_write` + validation, slots are already reserved.
- **No MQTT/cloud reporting**, no Eddystone-specific encoders (raw replay covers them).
- **Single-radio ceiling**: for guaranteed simultaneous scan + advertise + AP, add a second
  chip (ESP32-H2 as a BLE co-processor over UART/SPI).
- Optional future: wall-clock injection from the browser (`POST /api/time`) to timestamp
  archive entries, and a "capture feedback" blink so the operator knows a packet arrived
  without looking at the phone.

---

## 14. Compliance and safety

1. **Radio regulation**: 2.4 GHz low-power devices are type-approved per country
   (SRRC / CE / FCC). Do not exceed the module's certified output power.
2. **"iBeacon" is an Apple trademark.** Describe the product as *iBeacon compatible*.
3. **Replicating third-party identifiers is the real risk.** A replayed UUID/major/minor is
   indistinguishable from the original to downstream systems, which may be used for
   positioning, check-in or marketing. Restrict to owned assets or authorized engagements.
   The shipped default profile derives a private UUID from the MAC rather than reusing
   anyone's identifier.
4. **Interference**: many identical high-power beacons in one space degrade other
   installations. Assess before deployment.
5. **Duty cycle**: scan duty is capped (50 %) so that the management AP remains usable —
   which also keeps interference with neighbouring Wi-Fi bounded.
